/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

/*
 * Native Wayland window / input backend.
 *
 * Only the Vulkan renderer uses this backend.  The legacy OpenGL renderer
 * still needs GLX and therefore keeps using the X11 path (through XWayland
 * when the session itself is Wayland).
 */

#ifdef HAVE_WAYLAND

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <linux/input-event-codes.h>

#include "../client/client.h"
#include "linux_local.h"

#include "xdg-shell-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"

#include "linux_wayland.h"

extern cvar_t *in_nograb;
extern void Sys_Quit( void );

/* Implemented in linux_glimp.c and shared with the X11 backend. */
extern int XLateKeysym( unsigned long keysym );

static struct wl_display    *wl_dpy;
static struct wl_registry   *wl_registry;
static struct wl_compositor *wl_compositor;
static struct wl_shm        *wl_shm;
static struct xdg_wm_base   *wl_wm_base;
static struct wl_seat       *wl_seat;
static struct wl_keyboard   *wl_keyboard;
static struct wl_pointer    *wl_pointer;
static struct wl_surface    *wl_cursor_surface;
static struct wl_cursor_theme *wl_cursor_theme;
static struct wl_cursor     *wl_cursor;

static struct zwp_relative_pointer_manager_v1 *wl_rel_mgr;
static struct zwp_relative_pointer_v1         *wl_rel_ptr;
static struct zwp_pointer_constraints_v1      *wl_ptr_constraints;
static struct zwp_locked_pointer_v1            *wl_locked_ptr;

static struct wl_surface   *wl_surface;
static struct xdg_surface  *wl_xdg_surface;
static struct xdg_toplevel *wl_toplevel;

static struct xkb_context *xkb_ctx;
static struct xkb_keymap  *xkb_keymap;
static struct xkb_state   *xkb_state;

static qboolean wl_active;
static qboolean wl_configured;
static qboolean wl_running;
static qboolean wl_fullscreen;
static qboolean wl_pending_fullscreen;
static qboolean wl_mouse_active;
static qboolean wl_pointer_entered;

static int wl_width = 1280;
static int wl_height = 720;
static int wl_requested_width = 1280;
static int wl_requested_height = 720;
static int wl_pending_width;
static int wl_pending_height;

static uint32_t wl_pointer_serial;
static wl_fixed_t wl_last_x;
static wl_fixed_t wl_last_y;

static uint32_t wl_repeat_delay = 600;
static uint32_t wl_repeat_interval = 40;
static uint32_t wl_repeat_next;
static uint32_t wl_repeat_code;
static int      wl_repeat_key;
static int      wl_repeat_text[8];

static unsigned char wl_key_down[KEY_CNT];

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

static void wl_queue_key( int qkey, qboolean down )
{
	int t = Sys_Milliseconds();

	if ( qkey )
		Sys_QueEvent( t, SE_KEY, qkey, down, 0, NULL );
}

static void wl_queue_text( int t, const char *text )
{
	int i;

	if ( !text )
		return;

	for ( i = 0; text[i]; i++ )
	{
		unsigned char c = (unsigned char)text[i];

		if ( c == 127 )
			continue;
		/* Ctrl+letter arrives as a control character, just like X11. */
		if ( c >= 1 && c <= 26 )
			c = (unsigned char)( c + 'a' - 1 );
		if ( c >= 32 )
			Sys_QueEvent( t, SE_CHAR, c, 0, 0, NULL );
	}
}

static void wl_update_cursor( qboolean show )
{
	struct wl_cursor_image *image;

	if ( !wl_pointer || !wl_pointer_serial )
		return;

	if ( !show )
	{
		wl_pointer_set_cursor( wl_pointer, wl_pointer_serial, NULL, 0, 0 );
		return;
	}

	if ( !wl_cursor_theme && wl_shm )
		wl_cursor_theme = wl_cursor_theme_load( NULL, 24, wl_shm );

	if ( !wl_cursor_theme )
		return;

	if ( !wl_cursor )
		wl_cursor = wl_cursor_theme_get_cursor( wl_cursor_theme, "left_ptr" );

	if ( !wl_cursor || !wl_cursor->image_count )
		return;

	if ( !wl_cursor_surface )
		wl_cursor_surface = wl_compositor_create_surface( wl_compositor );

	image = wl_cursor->images[0];
	wl_surface_attach( wl_cursor_surface, wl_cursor_image_get_buffer( image ), 0, 0 );
	wl_surface_damage( wl_cursor_surface, 0, 0,
	                   (int)image->width, (int)image->height );
	wl_surface_commit( wl_cursor_surface );

	wl_pointer_set_cursor( wl_pointer, wl_pointer_serial, wl_cursor_surface,
	                       (int)image->hotspot_x, (int)image->hotspot_y );
}

/* ------------------------------------------------------------------------- */
/* keyboard                                                                  */
/* ------------------------------------------------------------------------- */

static void keyboard_keymap( void *data, struct wl_keyboard *keyboard,
                             uint32_t format, int32_t fd, uint32_t size )
{
	char *map;

	(void)data;
	(void)keyboard;

	if ( format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 )
	{
		close( fd );
		return;
	}

	map = mmap( NULL, size, PROT_READ, MAP_PRIVATE, fd, 0 );
	if ( map == MAP_FAILED )
	{
		close( fd );
		return;
	}

	if ( !xkb_ctx )
		xkb_ctx = xkb_context_new( XKB_CONTEXT_NO_FLAGS );

	if ( xkb_ctx )
	{
		struct xkb_keymap *new_map = xkb_keymap_new_from_string(
			xkb_ctx, map, XKB_KEYMAP_FORMAT_TEXT_V1,
			XKB_KEYMAP_COMPILE_NO_FLAGS );

		if ( new_map )
		{
			struct xkb_state *new_state = xkb_state_new( new_map );

			if ( new_state )
			{
				if ( xkb_state )
					xkb_state_unref( xkb_state );
				if ( xkb_keymap )
					xkb_keymap_unref( xkb_keymap );

				xkb_keymap = new_map;
				xkb_state = new_state;
			}
			else
			{
				xkb_keymap_unref( new_map );
			}
		}
	}

	munmap( map, size );
	close( fd );
}

static void keyboard_enter( void *data, struct wl_keyboard *keyboard,
                            uint32_t serial, struct wl_surface *surface,
                            struct wl_array *keys )
{
	uint32_t *k;

	(void)data;
	(void)keyboard;
	(void)serial;
	(void)surface;

	wl_array_for_each( k, keys )
	{
		if ( *k < KEY_CNT )
			wl_key_down[*k] = 1;
	}
}

static void keyboard_leave( void *data, struct wl_keyboard *keyboard,
                            uint32_t serial, struct wl_surface *surface )
{
	int i;

	(void)data;
	(void)keyboard;
	(void)serial;
	(void)surface;

	for ( i = 0; i < KEY_CNT; i++ )
	{
		if ( wl_key_down[i] )
		{
			xkb_keysym_t sym;
			int qkey;

			wl_key_down[i] = 0;
			if ( !xkb_state )
				continue;
			sym = xkb_state_key_get_one_sym( xkb_state, (xkb_keycode_t)( i + 8 ) );
			qkey = XLateKeysym( (unsigned long)sym );
			wl_queue_key( qkey, qfalse );
			xkb_state_update_key( xkb_state, (xkb_keycode_t)( i + 8 ),
			                      XKB_KEY_UP );
		}
	}

	wl_repeat_code = 0;
	wl_repeat_key = 0;
}

static void keyboard_key( void *data, struct wl_keyboard *keyboard,
                          uint32_t serial, uint32_t time, uint32_t key,
                          uint32_t state )
{
	xkb_keycode_t code;
	xkb_keysym_t sym;
	char text[32];
	int qkey;
	int n;
	qboolean down;

	(void)data;
	(void)keyboard;
	(void)serial;
	(void)time;

	if ( !xkb_state || key >= KEY_CNT )
		return;

	code = (xkb_keycode_t)( key + 8 );
	down = ( state == WL_KEYBOARD_KEY_STATE_PRESSED );

	sym = xkb_state_key_get_one_sym( xkb_state, code );
	qkey = XLateKeysym( (unsigned long)sym );

	text[0] = '\0';
	n = xkb_state_key_get_utf8( xkb_state, code, text, sizeof( text ) );
	if ( n <= 0 )
	{
		text[0] = '\0';
		n = 0;
	}

	if ( qkey == 0 && n > 0 )
	{
		/* Printable characters are their own key code in Quake 3. */
		unsigned char c = (unsigned char)text[0];

		if ( c >= 'A' && c <= 'Z' )
			c = (unsigned char)( c - 'A' + 'a' );
		qkey = c;
	}

	if ( down )
	{
		wl_key_down[key] = 1;
		wl_queue_key( qkey, qtrue );

		if ( text[0] )
			wl_queue_text( Sys_Milliseconds(), text );

		if ( qkey && qkey != K_SHIFT && qkey != K_CTRL && qkey != K_ALT )
		{
			int i;

			wl_repeat_code = key;
			wl_repeat_key = qkey;
			for ( i = 0; i < (int)sizeof( wl_repeat_text ) - 1 && text[i]; i++ )
				wl_repeat_text[i] = (unsigned char)text[i];
			wl_repeat_text[i] = 0;
			wl_repeat_next = (uint32_t)Sys_Milliseconds() + wl_repeat_delay;
		}
		else
		{
			wl_repeat_code = 0;
			wl_repeat_key = 0;
		}
	}
	else
	{
		wl_key_down[key] = 0;
		wl_queue_key( qkey, qfalse );
		if ( key == wl_repeat_code )
		{
			wl_repeat_code = 0;
			wl_repeat_key = 0;
		}
	}

	xkb_state_update_key( xkb_state, code,
	                      down ? XKB_KEY_DOWN : XKB_KEY_UP );
}

static void keyboard_modifiers( void *data, struct wl_keyboard *keyboard,
                                uint32_t serial, uint32_t mods_depressed,
                                uint32_t mods_latched, uint32_t mods_locked,
                                uint32_t group )
{
	(void)data;
	(void)keyboard;
	(void)serial;

	if ( xkb_state )
		xkb_state_update_mask( xkb_state, mods_depressed, mods_latched,
		                       mods_locked, 0, 0, group );
}

static void keyboard_repeat_info( void *data, struct wl_keyboard *keyboard,
                                  int32_t rate, int32_t delay )
{
	(void)data;
	(void)keyboard;

	if ( delay > 0 )
		wl_repeat_delay = (uint32_t)delay;
	if ( rate > 0 )
		wl_repeat_interval = (uint32_t)( 1000 / rate );
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_keymap,
	.enter = keyboard_enter,
	.leave = keyboard_leave,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
	.repeat_info = keyboard_repeat_info,
};

/* ------------------------------------------------------------------------- */
/* pointer                                                                   */
/* ------------------------------------------------------------------------- */

static void pointer_enter( void *data, struct wl_pointer *pointer,
                           uint32_t serial, struct wl_surface *surface,
                           wl_fixed_t sx, wl_fixed_t sy )
{
	(void)data;
	(void)surface;

	wl_pointer_serial = serial;
	wl_pointer_entered = qtrue;
	wl_last_x = sx;
	wl_last_y = sy;

	if ( wl_mouse_active )
		wl_update_cursor( qfalse );
	else
		wl_update_cursor( qtrue );
}

static void pointer_leave( void *data, struct wl_pointer *pointer,
                           uint32_t serial, struct wl_surface *surface )
{
	(void)data;
	(void)pointer;
	(void)serial;
	(void)surface;

	wl_pointer_entered = qfalse;
	wl_pointer_serial = 0;
}

static void pointer_motion( void *data, struct wl_pointer *pointer,
                            uint32_t time, wl_fixed_t sx, wl_fixed_t sy )
{
	int dx, dy;

	(void)data;
	(void)pointer;
	(void)time;

	if ( wl_mouse_active && !wl_locked_ptr )
	{
		dx = wl_fixed_to_int( sx ) - wl_fixed_to_int( wl_last_x );
		dy = wl_fixed_to_int( sy ) - wl_fixed_to_int( wl_last_y );

		if ( dx || dy )
			Sys_QueEvent( Sys_Milliseconds(), SE_MOUSE, dx, dy, 0, NULL );
	}

	wl_last_x = sx;
	wl_last_y = sy;
}

static void pointer_button( void *data, struct wl_pointer *pointer,
                            uint32_t serial, uint32_t time, uint32_t button,
                            uint32_t state )
{
	int b = -1;

	(void)data;
	(void)pointer;
	(void)serial;
	(void)time;

	switch ( button )
	{
	case BTN_LEFT:   b = 0; break;
	case BTN_RIGHT:  b = 1; break;
	case BTN_MIDDLE: b = 2; break;
	case BTN_SIDE:   b = 3; break;
	case BTN_EXTRA:  b = 4; break;
	case BTN_FORWARD: b = 3; break;
	case BTN_BACK:   b = 4; break;
	default: break;
	}

	if ( b >= 0 )
		Sys_QueEvent( Sys_Milliseconds(), SE_KEY, K_MOUSE1 + b,
		              state == WL_POINTER_BUTTON_STATE_PRESSED, 0, NULL );
}

static void pointer_axis( void *data, struct wl_pointer *pointer,
                          uint32_t time, uint32_t axis, wl_fixed_t value )
{
	int t;

	(void)data;
	(void)pointer;
	(void)time;

	if ( axis != WL_POINTER_AXIS_VERTICAL_SCROLL || value == 0 )
		return;

	t = Sys_Milliseconds();
	if ( value < 0 )
	{
		Sys_QueEvent( t, SE_KEY, K_MWHEELUP, qtrue, 0, NULL );
		Sys_QueEvent( t, SE_KEY, K_MWHEELUP, qfalse, 0, NULL );
	}
	else
	{
		Sys_QueEvent( t, SE_KEY, K_MWHEELDOWN, qtrue, 0, NULL );
		Sys_QueEvent( t, SE_KEY, K_MWHEELDOWN, qfalse, 0, NULL );
	}
}

static void pointer_frame( void *data, struct wl_pointer *pointer )
{
	(void)data;
	(void)pointer;
}

static void pointer_axis_source( void *data, struct wl_pointer *pointer,
                                 uint32_t axis_source )
{
	(void)data;
	(void)pointer;
	(void)axis_source;
}

static void pointer_axis_stop( void *data, struct wl_pointer *pointer,
                               uint32_t time, uint32_t axis )
{
	(void)data;
	(void)pointer;
	(void)time;
	(void)axis;
}

static void pointer_axis_discrete( void *data, struct wl_pointer *pointer,
                                   uint32_t axis, int32_t discrete )
{
	(void)data;
	(void)pointer;
	(void)axis;
	(void)discrete;
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
	.frame = pointer_frame,
	.axis_source = pointer_axis_source,
	.axis_stop = pointer_axis_stop,
	.axis_discrete = pointer_axis_discrete,
};

static void relative_pointer_motion( void *data,
                                     struct zwp_relative_pointer_v1 *rel,
                                     uint32_t utime_hi, uint32_t utime_lo,
                                     wl_fixed_t dx, wl_fixed_t dy,
                                     wl_fixed_t dx_unaccel, wl_fixed_t dy_unaccel )
{
	int ix, iy;

	(void)data;
	(void)rel;
	(void)utime_hi;
	(void)utime_lo;
	(void)dx_unaccel;
	(void)dy_unaccel;

	if ( !wl_mouse_active )
		return;

	ix = wl_fixed_to_int( dx );
	iy = wl_fixed_to_int( dy );

	if ( ix || iy )
		Sys_QueEvent( Sys_Milliseconds(), SE_MOUSE, ix, iy, 0, NULL );
}

static const struct zwp_relative_pointer_v1_listener relative_pointer_listener = {
	.relative_motion = relative_pointer_motion,
};

static void locked_pointer_locked( void *data, struct zwp_locked_pointer_v1 *locked )
{
	(void)data;
	(void)locked;
}

static void locked_pointer_unlocked( void *data, struct zwp_locked_pointer_v1 *locked )
{
	(void)data;
	(void)locked;
}

static const struct zwp_locked_pointer_v1_listener locked_pointer_listener = {
	.locked = locked_pointer_locked,
	.unlocked = locked_pointer_unlocked,
};

/* ------------------------------------------------------------------------- */
/* seat                                                                      */
/* ------------------------------------------------------------------------- */

static void seat_capabilities( void *data, struct wl_seat *seat, uint32_t caps )
{
	(void)data;

	if ( ( caps & WL_SEAT_CAPABILITY_KEYBOARD ) && !wl_keyboard )
	{
		wl_keyboard = wl_seat_get_keyboard( seat );
		wl_keyboard_add_listener( wl_keyboard, &keyboard_listener, NULL );
	}
	else if ( !( caps & WL_SEAT_CAPABILITY_KEYBOARD ) && wl_keyboard )
	{
		wl_keyboard_destroy( wl_keyboard );
		wl_keyboard = NULL;
	}

	if ( ( caps & WL_SEAT_CAPABILITY_POINTER ) && !wl_pointer )
	{
		wl_pointer = wl_seat_get_pointer( seat );
		wl_pointer_add_listener( wl_pointer, &pointer_listener, NULL );

		if ( wl_rel_mgr && !wl_rel_ptr )
		{
			wl_rel_ptr = zwp_relative_pointer_manager_v1_get_relative_pointer(
				wl_rel_mgr, wl_pointer );
			zwp_relative_pointer_v1_add_listener( wl_rel_ptr,
			                                      &relative_pointer_listener, NULL );
		}
	}
	else if ( !( caps & WL_SEAT_CAPABILITY_POINTER ) && wl_pointer )
	{
		wl_pointer_destroy( wl_pointer );
		wl_pointer = NULL;
	}
}

static void seat_name( void *data, struct wl_seat *seat, const char *name )
{
	(void)data;
	(void)seat;
	(void)name;
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

/* ------------------------------------------------------------------------- */
/* xdg-shell                                                                 */
/* ------------------------------------------------------------------------- */

static void wm_base_ping( void *data, struct xdg_wm_base *wm_base, uint32_t serial )
{
	(void)data;
	xdg_wm_base_pong( wm_base, serial );
}

static const struct xdg_wm_base_listener wm_base_listener = {
	.ping = wm_base_ping,
};

static void xdg_surface_configure( void *data, struct xdg_surface *xdg_surface,
                                   uint32_t serial )
{
	(void)data;

	xdg_surface_ack_configure( xdg_surface, serial );

	if ( wl_pending_width > 0 )
		wl_width = wl_pending_width;
	else if ( !wl_fullscreen )
		wl_width = wl_requested_width;

	if ( wl_pending_height > 0 )
		wl_height = wl_pending_height;
	else if ( !wl_fullscreen )
		wl_height = wl_requested_height;

	/* In fullscreen mode the compositor first sends an ordinary configure
	 * and then the real fullscreen one.  Creating the swapchain for the
	 * first size would make it immediately out of date. */
	if ( wl_fullscreen && !wl_pending_fullscreen )
		return;

	/* The first accepted commit maps the xdg surface; afterwards the Vulkan
	 * WSI owns commits and we must not race with presentation. */
	if ( !wl_configured )
		wl_surface_commit( wl_surface );

	wl_configured = qtrue;
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void toplevel_configure( void *data, struct xdg_toplevel *toplevel,
                                int32_t width, int32_t height,
                                struct wl_array *states )
{
	uint32_t *state;

	(void)data;
	(void)toplevel;

	wl_pending_fullscreen = qfalse;
	wl_array_for_each( state, states )
	{
		if ( *state == XDG_TOPLEVEL_STATE_FULLSCREEN )
			wl_pending_fullscreen = qtrue;
	}

	if ( width > 0 )
		wl_pending_width = width;
	if ( height > 0 )
		wl_pending_height = height;
}

static void toplevel_close( void *data, struct xdg_toplevel *toplevel )
{
	(void)data;
	(void)toplevel;

	wl_running = qfalse;
}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
};

/* ------------------------------------------------------------------------- */
/* registry                                                                  */
/* ------------------------------------------------------------------------- */

static void registry_global( void *data, struct wl_registry *registry,
                             uint32_t name, const char *interface,
                             uint32_t version )
{
	(void)data;

	if ( strcmp( interface, wl_compositor_interface.name ) == 0 )
	{
		wl_compositor = wl_registry_bind( registry, name,
		                                 &wl_compositor_interface,
		                                 version < 4 ? version : 4 );
	}
	else if ( strcmp( interface, wl_shm_interface.name ) == 0 )
	{
		wl_shm = wl_registry_bind( registry, name, &wl_shm_interface, 1 );
	}
	else if ( strcmp( interface, wl_seat_interface.name ) == 0 )
	{
		wl_seat = wl_registry_bind( registry, name, &wl_seat_interface,
		                            version < 7 ? version : 7 );
		wl_seat_add_listener( wl_seat, &seat_listener, NULL );
	}
	else if ( strcmp( interface, xdg_wm_base_interface.name ) == 0 )
	{
		wl_wm_base = wl_registry_bind( registry, name,
		                               &xdg_wm_base_interface,
		                               version < 2 ? version : 2 );
		xdg_wm_base_add_listener( wl_wm_base, &wm_base_listener, NULL );
	}
	else if ( strcmp( interface, zwp_relative_pointer_manager_v1_interface.name ) == 0 )
	{
		wl_rel_mgr = wl_registry_bind( registry, name,
		                               &zwp_relative_pointer_manager_v1_interface, 1 );
	}
	else if ( strcmp( interface, zwp_pointer_constraints_v1_interface.name ) == 0 )
	{
		wl_ptr_constraints = wl_registry_bind( registry, name,
		                                       &zwp_pointer_constraints_v1_interface, 1 );
	}
}

static void registry_global_remove( void *data, struct wl_registry *registry,
                                    uint32_t name )
{
	(void)data;
	(void)registry;
	(void)name;
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

/* ------------------------------------------------------------------------- */
/* public API                                                                */
/* ------------------------------------------------------------------------- */

qboolean WL_Init( int width, int height, qboolean fullscreen, const char *title )
{
	struct pollfd pfd;
	int wait_ms;

	if ( wl_active )
		return qtrue;

	if ( !getenv( "WAYLAND_DISPLAY" ) && !getenv( "WAYLAND_SOCKET" ) )
		return qfalse;

	wl_dpy = wl_display_connect( NULL );
	if ( !wl_dpy )
	{
		Com_Printf( "Wayland: could not connect to compositor\n" );
		return qfalse;
	}

	wl_requested_width = width > 0 ? width : 1280;
	wl_requested_height = height > 0 ? height : 720;
	wl_width = wl_requested_width;
	wl_height = wl_requested_height;
	wl_fullscreen = fullscreen;
	wl_running = qtrue;

	wl_registry = wl_display_get_registry( wl_dpy );
	wl_registry_add_listener( wl_registry, &registry_listener, NULL );

	/* First roundtrip: bind globals.  Second: receive seat capabilities. */
	if ( wl_display_roundtrip( wl_dpy ) < 0 ||
	     wl_display_roundtrip( wl_dpy ) < 0 )
	{
		Com_Printf( "Wayland: failed to initialise registry\n" );
		WL_Shutdown();
		return qfalse;
	}

	if ( !wl_compositor || !wl_wm_base )
	{
		Com_Printf( "Wayland: compositor lacks xdg-shell\n" );
		WL_Shutdown();
		return qfalse;
	}

	wl_surface = wl_compositor_create_surface( wl_compositor );
	if ( !wl_surface )
	{
		Com_Printf( "Wayland: could not create surface\n" );
		WL_Shutdown();
		return qfalse;
	}

	wl_xdg_surface = xdg_wm_base_get_xdg_surface( wl_wm_base, wl_surface );
	xdg_surface_add_listener( wl_xdg_surface, &xdg_surface_listener, NULL );

	wl_toplevel = xdg_surface_get_toplevel( wl_xdg_surface );
	xdg_toplevel_add_listener( wl_toplevel, &toplevel_listener, NULL );
	xdg_toplevel_set_title( wl_toplevel, title ? title : "Quake III: Arena" );
	xdg_toplevel_set_app_id( wl_toplevel, "quake3" );

	if ( wl_fullscreen )
	{
		xdg_toplevel_set_fullscreen( wl_toplevel, NULL );
	}
	else
	{
		xdg_toplevel_set_min_size( wl_toplevel, wl_requested_width, wl_requested_height );
		xdg_toplevel_set_max_size( wl_toplevel, wl_requested_width, wl_requested_height );
	}

	wl_surface_commit( wl_surface );
	wl_display_flush( wl_dpy );

	/* Wait for the initial configure (bounded, so a broken compositor cannot
	 * hang the game during startup). */
	for ( wait_ms = 0; !wl_configured && wait_ms < 5000; wait_ms += 100 )
	{
		pfd.fd = wl_display_get_fd( wl_dpy );
		pfd.events = POLLIN;
		pfd.revents = 0;

		if ( poll( &pfd, 1, 100 ) > 0 )
		{
			if ( wl_display_dispatch( wl_dpy ) < 0 )
				break;
		}
	}

	if ( !wl_configured )
	{
		Com_Printf( "Wayland: no xdg_surface configure received; falling back\n" );
		WL_Shutdown();
		return qfalse;
	}

	wl_active = qtrue;

	Com_Printf( "Wayland: native window %dx%d (fullscreen=%s), app_id quake3\n",
	            wl_width, wl_height, wl_fullscreen ? "yes" : "no" );

	return qtrue;
}

void WL_Shutdown( void )
{
	if ( wl_locked_ptr )
	{
		zwp_locked_pointer_v1_destroy( wl_locked_ptr );
		wl_locked_ptr = NULL;
	}
	if ( wl_rel_ptr )
	{
		zwp_relative_pointer_v1_destroy( wl_rel_ptr );
		wl_rel_ptr = NULL;
	}
	if ( wl_keyboard )
	{
		wl_keyboard_destroy( wl_keyboard );
		wl_keyboard = NULL;
	}
	if ( wl_pointer )
	{
		wl_pointer_destroy( wl_pointer );
		wl_pointer = NULL;
	}
	if ( wl_cursor_surface )
	{
		wl_surface_destroy( wl_cursor_surface );
		wl_cursor_surface = NULL;
	}
	if ( wl_cursor_theme )
	{
		wl_cursor_theme_destroy( wl_cursor_theme );
		wl_cursor_theme = NULL;
	}
	wl_cursor = NULL;
	if ( wl_toplevel )
	{
		xdg_toplevel_destroy( wl_toplevel );
		wl_toplevel = NULL;
	}
	if ( wl_xdg_surface )
	{
		xdg_surface_destroy( wl_xdg_surface );
		wl_xdg_surface = NULL;
	}
	if ( wl_surface )
	{
		wl_surface_destroy( wl_surface );
		wl_surface = NULL;
	}
	if ( wl_ptr_constraints )
	{
		zwp_pointer_constraints_v1_destroy( wl_ptr_constraints );
		wl_ptr_constraints = NULL;
	}
	if ( wl_rel_mgr )
	{
		zwp_relative_pointer_manager_v1_destroy( wl_rel_mgr );
		wl_rel_mgr = NULL;
	}
	if ( wl_seat )
	{
		wl_seat_destroy( wl_seat );
		wl_seat = NULL;
	}
	if ( wl_wm_base )
	{
		xdg_wm_base_destroy( wl_wm_base );
		wl_wm_base = NULL;
	}
	if ( wl_shm )
	{
		wl_shm_destroy( wl_shm );
		wl_shm = NULL;
	}
	if ( wl_compositor )
	{
		wl_compositor_destroy( wl_compositor );
		wl_compositor = NULL;
	}
	if ( wl_registry )
	{
		wl_registry_destroy( wl_registry );
		wl_registry = NULL;
	}
	if ( xkb_state )
	{
		xkb_state_unref( xkb_state );
		xkb_state = NULL;
	}
	if ( xkb_keymap )
	{
		xkb_keymap_unref( xkb_keymap );
		xkb_keymap = NULL;
	}
	if ( xkb_ctx )
	{
		xkb_context_unref( xkb_ctx );
		xkb_ctx = NULL;
	}
	if ( wl_dpy )
	{
		wl_display_flush( wl_dpy );
		wl_display_disconnect( wl_dpy );
		wl_dpy = NULL;
	}

	wl_active = qfalse;
	wl_configured = qfalse;
	wl_mouse_active = qfalse;
	wl_pointer_entered = qfalse;
	wl_pointer_serial = 0;
	wl_repeat_code = 0;
	wl_repeat_key = 0;
	memset( wl_key_down, 0, sizeof( wl_key_down ) );
}

qboolean WL_IsActive( void )
{
	return wl_active;
}

void WL_Pump( void )
{
	struct pollfd pfd;
	qboolean prepared = qfalse;

	if ( !wl_active || !wl_dpy )
		return;

	for ( ;; )
	{
		if ( wl_display_prepare_read( wl_dpy ) == 0 )
		{
			prepared = qtrue;
			break;
		}
		if ( wl_display_dispatch_pending( wl_dpy ) < 0 )
		{
			wl_running = qfalse;
			break;
		}
	}

	if ( wl_running )
	{
		wl_display_flush( wl_dpy );

		pfd.fd = wl_display_get_fd( wl_dpy );
		pfd.events = POLLIN;
		pfd.revents = 0;

		if ( poll( &pfd, 1, 0 ) > 0 )
		{
			if ( wl_display_read_events( wl_dpy ) < 0 ||
			     wl_display_dispatch_pending( wl_dpy ) < 0 )
				wl_running = qfalse;
		}
		else if ( prepared )
		{
			wl_display_cancel_read( wl_dpy );
		}
	}

	/* Client-side key repeat: Wayland deliberately leaves this to clients. */
	if ( wl_repeat_code && wl_repeat_key )
	{
		uint32_t t = (uint32_t)Sys_Milliseconds();

		if ( t >= wl_repeat_next )
		{
			wl_queue_key( wl_repeat_key, qtrue );
			if ( wl_repeat_text[0] )
			{
				char text[sizeof( wl_repeat_text )];
				int i;

				for ( i = 0; i < (int)sizeof( wl_repeat_text ); i++ )
					text[i] = (char)wl_repeat_text[i];

				wl_queue_text( (int)t, text );
			}
			wl_repeat_next = t + wl_repeat_interval;
		}
	}

	if ( !wl_running )
	{
		wl_running = qtrue;   /* avoid re-entering this path */
		Sys_Quit();
	}
}

void WL_ActivateMouse( void )
{
	if ( !wl_active || wl_mouse_active )
		return;

	if ( ( !in_nograb || !in_nograb->value ) &&
	     wl_ptr_constraints && wl_pointer && wl_surface && !wl_locked_ptr )
	{
		wl_locked_ptr = zwp_pointer_constraints_v1_lock_pointer(
			wl_ptr_constraints, wl_surface, wl_pointer, NULL,
			ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT );

		if ( wl_locked_ptr )
			zwp_locked_pointer_v1_add_listener( wl_locked_ptr,
			                                    &locked_pointer_listener, NULL );
	}

	wl_mouse_active = qtrue;
	wl_update_cursor( qfalse );
}

void WL_DeactivateMouse( void )
{
	if ( !wl_active )
		return;

	if ( wl_locked_ptr )
	{
		zwp_locked_pointer_v1_destroy( wl_locked_ptr );
		wl_locked_ptr = NULL;
	}

	wl_mouse_active = qfalse;
	wl_update_cursor( qtrue );
}

void *WL_GetDisplay( void )
{
	return wl_dpy;
}

void *WL_GetSurface( void )
{
	return wl_surface;
}

int WL_GetWidth( void )
{
	return wl_width;
}

int WL_GetHeight( void )
{
	return wl_height;
}

#else  /* !HAVE_WAYLAND */

#include "linux_wayland.h"

qboolean WL_Init( int width, int height, qboolean fullscreen, const char *title )
{
	(void)width; (void)height; (void)fullscreen; (void)title;
	return qfalse;
}
void WL_Shutdown( void ) {}
qboolean WL_IsActive( void ) { return qfalse; }
void WL_Pump( void ) {}
void WL_ActivateMouse( void ) {}
void WL_DeactivateMouse( void ) {}
void *WL_GetDisplay( void ) { return NULL; }
void *WL_GetSurface( void ) { return NULL; }
int WL_GetWidth( void ) { return 0; }
int WL_GetHeight( void ) { return 0; }

#endif
