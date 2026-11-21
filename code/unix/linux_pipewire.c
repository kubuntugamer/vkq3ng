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
 * PipeWire DMA backend.
 *
 * The engine writes into a software ring buffer (dma.buffer) exactly like it
 * did with the OSS mmap backend.  A PipeWire real-time process callback
 * consumes that ring and advances the virtual DMA position.  SNDDMA_Submit()
 * therefore has nothing to do: the data is already visible to the consumer.
 */

#include <stdatomic.h>
#include <string.h>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include "../game/q_shared.h"
#include "../client/snd_local.h"

cvar_t *sndbits;
cvar_t *sndspeed;
cvar_t *sndchannels;

cvar_t *snddevice;

static qboolean snd_inited = qfalse;

static struct pw_thread_loop *pw_loop;
static struct pw_stream      *pw_stream;
static int16_t               *ring;
static _Atomic uint32_t       play_pos;      /* interleaved samples */
static qboolean               stream_error;

/* glibc memset workaround, kept for the engine's Snd_Memset hook */
void Snd_Memset (void* dest, const int val, const size_t count)
{
	Com_Memset(dest, val, count);
}

static void on_stream_state_changed(void *userdata, enum pw_stream_state old,
                                    enum pw_stream_state state, const char *error)
{
	(void)userdata;
	(void)old;

	if (state == PW_STREAM_STATE_ERROR) {
		stream_error = qtrue;
		Com_Printf("PipeWire stream error: %s\n", error ? error : "unknown");
	}
}

static void on_process(void *userdata)
{
	struct pw_buffer *b;
	struct spa_buffer *buf;
	int16_t *dst;
	uint32_t stride, max_frames, frames, pos, i;
	int c;

	(void)userdata;

	if (!pw_stream || !ring)
		return;

	b = pw_stream_dequeue_buffer(pw_stream);
	if (!b)
		return;

	buf = b->buffer;
	if (!buf || buf->n_datas < 1 || !buf->datas[0].data) {
		pw_stream_queue_buffer(pw_stream, b);
		return;
	}

	stride = sizeof(int16_t) * dma.channels;
	max_frames = buf->datas[0].maxsize / stride;
	frames = b->requested > 0 ? (uint32_t)b->requested : max_frames;
	if (frames > max_frames)
		frames = max_frames;

	dst = (int16_t *)buf->datas[0].data;
	pos = atomic_load_explicit(&play_pos, memory_order_relaxed);
	if (pos >= (uint32_t)dma.samples)
		pos %= (uint32_t)dma.samples;

	for (i = 0; i < frames; i++) {
		for (c = 0; c < dma.channels; c++) {
			dst[i * dma.channels + c] = ring[pos];
			if (++pos >= (uint32_t)dma.samples)
				pos = 0;
		}
	}

	buf->datas[0].chunk->offset = 0;
	buf->datas[0].chunk->stride = stride;
	buf->datas[0].chunk->size   = frames * stride;

	atomic_store_explicit(&play_pos, pos, memory_order_relaxed);
	pw_stream_queue_buffer(pw_stream, b);
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_stream_state_changed,
	.process = on_process,
};

static void pw_cleanup(void)
{
	if (pw_stream) {
		pw_stream_disconnect(pw_stream);
		pw_stream_destroy(pw_stream);
		pw_stream = NULL;
	}
	if (pw_loop) {
		pw_thread_loop_stop(pw_loop);
		pw_thread_loop_destroy(pw_loop);
		pw_loop = NULL;
	}
	pw_deinit();
	free(ring);
	ring = NULL;
	atomic_store(&play_pos, 0);
}

qboolean SNDDMA_Init(void)
{
	struct pw_properties *props;
	struct spa_audio_info_raw info;
	uint8_t pod_buffer[1024];
	struct spa_pod_builder pod_builder;
	const struct spa_pod *params[1];
	uint32_t ring_frames;
	int rate, channels;

	if (snd_inited)
		return qtrue;

	sndbits = Cvar_Get("sndbits", "16", CVAR_ARCHIVE);
	sndspeed = Cvar_Get("sndspeed", "0", CVAR_ARCHIVE);
	sndchannels = Cvar_Get("sndchannels", "2", CVAR_ARCHIVE);
	snddevice = Cvar_Get("snddevice", "pipewire", CVAR_ARCHIVE);

	/* 16-bit is what the mixer always produces on this platform. */
	dma.samplebits = 16;

	channels = (int)sndchannels->value;
	if (channels < 1 || channels > 2)
		channels = 2;
	dma.channels = channels;

	rate = (int)sndspeed->value;
	if (rate <= 0)
		rate = 48000;
	dma.speed = rate;

	/* 32768 frames is comfortably larger than s_mixahead at any rate. */
	ring_frames = 32768;
	dma.samples = (int)(ring_frames * channels);
	dma.submission_chunk = 1;

	ring = (int16_t *)calloc((size_t)dma.samples, sizeof(int16_t));
	if (!ring) {
		Com_Printf("PipeWire: out of memory for DMA ring\n");
		return qfalse;
	}
	dma.buffer = (byte *)ring;
	atomic_store(&play_pos, 0);
	stream_error = qfalse;

	pw_init(NULL, NULL);

	pw_loop = pw_thread_loop_new("quake3-audio", NULL);
	if (!pw_loop) {
		Com_Printf("PipeWire: could not create thread loop\n");
		pw_cleanup();
		return qfalse;
	}

	pw_thread_loop_lock(pw_loop);

	props = pw_properties_new(
		PW_KEY_MEDIA_TYPE,     "Audio",
		PW_KEY_MEDIA_CATEGORY, "Playback",
		PW_KEY_MEDIA_ROLE,     "Game",
		PW_KEY_APP_NAME,       "Quake III Arena",
		PW_KEY_NODE_NAME,      "quake3",
		NULL);

	pw_stream = pw_stream_new_simple(pw_thread_loop_get_loop(pw_loop),
	                                 "Quake III Arena", props,
	                                 &stream_events, NULL);
	if (!pw_stream) {
		Com_Printf("PipeWire: could not create stream\n");
		pw_thread_loop_unlock(pw_loop);
		pw_cleanup();
		return qfalse;
	}

	info.format = SPA_AUDIO_FORMAT_S16;
	info.rate = (uint32_t)dma.speed;
	info.channels = (uint32_t)dma.channels;
	if (dma.channels == 1) {
		info.position[0] = SPA_AUDIO_CHANNEL_MONO;
	} else {
		info.position[0] = SPA_AUDIO_CHANNEL_FL;
		info.position[1] = SPA_AUDIO_CHANNEL_FR;
	}

	pod_builder = SPA_POD_BUILDER_INIT(pod_buffer, sizeof(pod_buffer));
	params[0] = spa_format_audio_raw_build(&pod_builder,
	                                       SPA_PARAM_EnumFormat, &info);

	if (pw_stream_connect(pw_stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
	                      PW_STREAM_FLAG_AUTOCONNECT |
	                      PW_STREAM_FLAG_MAP_BUFFERS |
	                      PW_STREAM_FLAG_RT_PROCESS,
	                      params, 1) < 0) {
		Com_Printf("PipeWire: could not connect stream\n");
		pw_thread_loop_unlock(pw_loop);
		pw_cleanup();
		return qfalse;
	}

	pw_thread_loop_start(pw_loop);
	pw_thread_loop_unlock(pw_loop);

	snd_inited = qtrue;
	Com_Printf("PipeWire: %d Hz, %d channel(s), %d sample ring\n",
	           dma.speed, dma.channels, dma.samples);
	return qtrue;
}

int SNDDMA_GetDMAPos(void)
{
	uint32_t pos;

	if (!snd_inited)
		return 0;

	pos = atomic_load_explicit(&play_pos, memory_order_relaxed);
	return (int)(pos % (uint32_t)dma.samples);
}

void SNDDMA_Shutdown(void)
{
	if (!snd_inited)
		return;

	pw_cleanup();
	snd_inited = qfalse;
	dma.buffer = NULL;
	dma.samples = 0;
}

void SNDDMA_Submit(void)
{
}

void SNDDMA_BeginPainting(void)
{
}
