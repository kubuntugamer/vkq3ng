/*
===========================================================================
Copyright (C) 2025 vkq3ng contributors

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.
===========================================================================
*/

/*
 * Asynchronous image decoding for the renderer.
 *
 * JPG/PNG paths found in the shader text are queued here.  Worker threads
 * read the file and decode it to RGBA while the main thread continues with
 * shader parsing/upload.  R_LoadJPG/R_LoadPNG consume a completed decode
 * instead of doing the CPU-heavy decode in the foreground.  Foreground code
 * never waits for a worker: if the image is not ready it loads synchronously
 * and marks the queued entry dropped.  Completed decodes are cached across
 * map loads.
 */

#include "tr_local.h"

#ifndef _WIN32

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

extern int FS_ReadFileThreaded( const char *qpath, void **buffer );

#define ASYNC_IMAGE_MAX_THREADS	6
#define ASYNC_IMAGE_MAX_BYTES	(768u * 1024u * 1024u)
#define ASYNC_IMAGE_MAX_PATH	256

typedef enum {
	ASYNC_PENDING,
	ASYNC_RUNNING,
	ASYNC_DONE,
	ASYNC_CACHED,
	ASYNC_FAILED,
	ASYNC_DROPPED
} asyncImageState_t;

typedef struct asyncImage_s {
	char					*path;
	byte					*pic;
	int						width;
	int						height;
	asyncImageState_t		state;
	struct asyncImage_s		*next;
} asyncImage_t;

static pthread_mutex_t	asyncMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t	asyncCond = PTHREAD_COND_INITIALIZER;
static pthread_t		asyncThreads[ASYNC_IMAGE_MAX_THREADS];
static int				asyncThreadCount;
static qboolean			asyncStarted;
static qboolean			asyncStop;
static asyncImage_t		*asyncList;
static size_t			asyncBytes;

static asyncImage_t *R_AsyncFindLocked( const char *path ) {
	asyncImage_t *e;
	for ( e = asyncList; e; e = e->next ) {
		if ( !strcmp( e->path, path ) ) {
			return e;
		}
	}
	return NULL;
}

/* Drop the oldest decoded images once the cache reaches its budget. */
static void R_AsyncEvictLocked( void ) {
	while ( asyncBytes > ASYNC_IMAGE_MAX_BYTES ) {
		asyncImage_t *e, *victim = NULL, *victimPrev = NULL, *prev = NULL;

		for ( e = asyncList; e; prev = e, e = e->next ) {
			if ( e->state == ASYNC_CACHED ) {
				victim = e;
				victimPrev = prev;
			}
		}
		if ( !victim ) {
			break;
		}
		if ( victimPrev ) {
			victimPrev->next = victim->next;
		} else {
			asyncList = victim->next;
		}
		asyncBytes -= (size_t)victim->width * victim->height * 4;
		free( victim->pic );
		free( victim->path );
		free( victim );
	}
}

static void *R_AsyncImageWorker( void *unused ) {
	(void)unused;

	for ( ;; ) {
		asyncImage_t *e;
		char *path;
		void *raw = NULL;
		byte *pic = NULL;
		int width = 0, height = 0;
		int len;

		pthread_mutex_lock( &asyncMutex );

		R_AsyncEvictLocked();
		while ( !asyncStop && asyncBytes > ASYNC_IMAGE_MAX_BYTES ) {
			pthread_cond_wait( &asyncCond, &asyncMutex );
			R_AsyncEvictLocked();
		}

		for ( e = asyncList; e; e = e->next ) {
			if ( e->state == ASYNC_PENDING ) {
				break;
			}
		}

		if ( asyncStop || !e ) {
			pthread_mutex_unlock( &asyncMutex );
			break;
		}

		e->state = ASYNC_RUNNING;
		path = e->path;
		pthread_mutex_unlock( &asyncMutex );

		len = FS_ReadFileThreaded( path, &raw );
		if ( len >= 0 && raw ) {
			pic = R_DecodeImageMemory( path, raw, len, &width, &height );
		}
		free( raw );

		pthread_mutex_lock( &asyncMutex );

		if ( asyncStop || e->state != ASYNC_RUNNING ) {
			free( pic );
			pic = NULL;
			if ( e->state == ASYNC_RUNNING ) {
				e->state = ASYNC_DROPPED;
			}
		} else if ( pic ) {
			e->pic = pic;
			e->width = width;
			e->height = height;
			e->state = ASYNC_DONE;
			asyncBytes += (size_t)width * height * 4;
		} else {
			e->state = ASYNC_FAILED;
		}

		pthread_cond_broadcast( &asyncCond );
		pthread_mutex_unlock( &asyncMutex );
	}

	return NULL;
}

static void R_AsyncStart( void ) {
	int cores;
	int i;

	if ( asyncStarted ) {
		return;
	}
	asyncStarted = qtrue;

	if ( getenv( "Q3_NO_ASYNC_IMAGES" ) ) {
		return;
	}

	cores = Sys_ProcessorCount();
	if ( cores < 2 ) {
		return;
	}

	asyncThreadCount = cores >= 8 ? 6 : (cores >= 4 ? 3 : 1);
	if ( asyncThreadCount > ASYNC_IMAGE_MAX_THREADS ) {
		asyncThreadCount = ASYNC_IMAGE_MAX_THREADS;
	}

	for ( i = 0; i < asyncThreadCount; i++ ) {
		if ( pthread_create( &asyncThreads[i], NULL, R_AsyncImageWorker, NULL ) != 0 ) {
			break;
		}
	}
	asyncThreadCount = i;
}

static qboolean R_IsAsyncImagePath( const char *path ) {
	int len;

	if ( !path || ( len = (int)strlen( path ) ) < 5 || len >= ASYNC_IMAGE_MAX_PATH ) {
		return qfalse;
	}
	return ( !Q_stricmp( path + len - 4, ".jpg" ) ||
	         !Q_stricmp( path + len - 4, ".png" ) ) ? qtrue : qfalse;
}

void R_AsyncImagePrefetch( const char *path ) {
	asyncImage_t *e;

	if ( !R_IsAsyncImagePath( path ) ) {
		return;
	}

	R_AsyncStart();
	if ( !asyncThreadCount ) {
		return;
	}

	pthread_mutex_lock( &asyncMutex );
	{
		asyncImage_t *existing = R_AsyncFindLocked( path );
		if ( existing ) {
			if ( existing->state == ASYNC_DROPPED ) {
				existing->state = ASYNC_PENDING;
				pthread_cond_broadcast( &asyncCond );
			}
			pthread_mutex_unlock( &asyncMutex );
			return;
		}
	}

	e = calloc( 1, sizeof( *e ) );
	if ( !e || !( e->path = strdup( path ) ) ) {
		free( e );
		pthread_mutex_unlock( &asyncMutex );
		return;
	}
	e->state = ASYNC_PENDING;
	e->next = asyncList;
	asyncList = e;

	pthread_cond_broadcast( &asyncCond );
	pthread_mutex_unlock( &asyncMutex );
}

qboolean R_AsyncImageTake( const char *path, byte **pic, int *width, int *height ) {
	asyncImage_t *e;
	size_t size;

	if ( !asyncStarted || !path ) {
		return qfalse;
	}

	pthread_mutex_lock( &asyncMutex );
	e = R_AsyncFindLocked( path );
	if ( !e ) {
		pthread_mutex_unlock( &asyncMutex );
		return qfalse;
	}

	if ( e->state != ASYNC_DONE && e->state != ASYNC_CACHED ) {
		if ( e->state == ASYNC_PENDING || e->state == ASYNC_RUNNING ) {
			/* Do not stall the foreground for an opportunistic decode. */
			e->state = ASYNC_DROPPED;
			pthread_cond_broadcast( &asyncCond );
		}
		pthread_mutex_unlock( &asyncMutex );
		return qfalse;
	}

	size = (size_t)e->width * e->height * 4;
	*pic = (byte *)ri.Malloc( (int)size );
	if ( *pic ) {
		Com_Memcpy( *pic, e->pic, size );
		*width = e->width;
		*height = e->height;
		/* Keep the decoded pixels cached for the next map/reload. */
		e->state = ASYNC_CACHED;
		R_AsyncEvictLocked();
		pthread_cond_broadcast( &asyncCond );
	}

	pthread_mutex_unlock( &asyncMutex );
	return *pic != NULL;
}

void R_AsyncImageShutdown( void ) {
	asyncImage_t *e, *next;
	int i;

	if ( !asyncStarted ) {
		return;
	}

	pthread_mutex_lock( &asyncMutex );
	asyncStop = qtrue;
	pthread_cond_broadcast( &asyncCond );
	pthread_mutex_unlock( &asyncMutex );

	for ( i = 0; i < asyncThreadCount; i++ ) {
		pthread_join( asyncThreads[i], NULL );
	}

	for ( e = asyncList; e; e = next ) {
		next = e->next;
		free( e->pic );
		free( e->path );
		free( e );
	}

	asyncList = NULL;
	asyncBytes = 0;
	asyncThreadCount = 0;
	asyncStarted = qfalse;
	asyncStop = qfalse;
}

#else /* _WIN32 */

void R_AsyncImagePrefetch( const char *path ) { (void)path; }
qboolean R_AsyncImageTake( const char *path, byte **pic, int *width, int *height ) {
	(void)path;
	*pic = NULL;
	*width = *height = 0;
	return qfalse;
}
void R_AsyncImageShutdown( void ) {}

#endif
