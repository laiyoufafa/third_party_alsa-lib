/*
 *  Rate conversion Plug-In
 *  Copyright (c) 1999 by Jaroslav Kysela <perex@suse.cz>
 *
 *
 *   This library is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU Library General Public License as
 *   published by the Free Software Foundation; either version 2 of
 *   the License, or (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU Library General Public License for more details.
 *
 *   You should have received a copy of the GNU Library General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */
  
#ifdef __KERNEL__
#include "../../include/driver.h"
#include "../../include/pcm.h"
#include "../../include/pcm_plugin.h"
#else
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <endian.h>
#include <byteswap.h>
#include "../pcm_local.h"
#endif

#define SHIFT	11
#define BITS	(1<<SHIFT)
#define MASK	(BITS-1)

/*
 *  Basic rate conversion plugin
 */

typedef struct {
	signed short last_S1;
	signed short last_S2;
} rate_voice_t;
 
typedef void (*rate_f)(snd_pcm_plugin_t *plugin,
		       const snd_pcm_plugin_voice_t *src_voices,
		       snd_pcm_plugin_voice_t *dst_voices,
		       int src_frames, int dst_frames);

typedef struct rate_private_data {
	unsigned int pitch;
	unsigned int pos;
	rate_f func;
	int get, put;
	ssize_t old_src_frames, old_dst_frames;
	rate_voice_t voices[0];
} rate_t;

static void rate_init(snd_pcm_plugin_t *plugin)
{
	unsigned int voice;
	rate_t *data = (rate_t *)plugin->extra_data;
	data->pos = 0;
	for (voice = 0; voice < plugin->src_format.voices; voice++) {
		data->voices[voice].last_S1 = 0;
		data->voices[voice].last_S2 = 0;
	}
}

static void resample_expand(snd_pcm_plugin_t *plugin,
			    const snd_pcm_plugin_voice_t *src_voices,
			    snd_pcm_plugin_voice_t *dst_voices,
			    int src_frames, int dst_frames)
{
	unsigned int pos = 0;
	signed int val;
	signed short S1, S2;
	char *src, *dst;
	unsigned int voice;
	int src_step, dst_step;
	int src_frames1, dst_frames1;
	rate_t *data = (rate_t *)plugin->extra_data;
	rate_voice_t *rvoices = data->voices;

#define GET_S16_LABELS
#define PUT_S16_LABELS
#include "plugin_ops.h"
#undef GET_S16_LABELS
#undef PUT_S16_LABELS
	void *get = get_s16_labels[data->get];
	void *put = put_s16_labels[data->put];
	void *get_s16_end = 0;
	signed short sample = 0;
#define GET_S16_END *get_s16_end
#include "plugin_ops.h"
#undef GET_S16_END
	
	for (voice = 0; voice < plugin->src_format.voices; voice++, rvoices++) {
		pos = data->pos;
		S1 = rvoices->last_S1;
		S2 = rvoices->last_S2;
		if (!src_voices[voice].enabled) {
			if (dst_voices[voice].wanted)
				snd_pcm_area_silence(&dst_voices[voice].area, 0, dst_frames, plugin->dst_format.format);
			dst_voices[voice].enabled = 0;
			continue;
		}
		dst_voices[voice].enabled = 1;
		src = (char *)src_voices[voice].area.addr + src_voices[voice].area.first / 8;
		dst = (char *)dst_voices[voice].area.addr + dst_voices[voice].area.first / 8;
		src_step = src_voices[voice].area.step / 8;
		dst_step = dst_voices[voice].area.step / 8;
		src_frames1 = src_frames;
		dst_frames1 = dst_frames;
		if (pos & ~MASK) {
			get_s16_end = &&after_get1;
			goto *get;
		after_get1:
			pos &= MASK;
			S1 = S2;
			S2 = sample;
			src += src_step;
			src_frames--;
		}
		while (dst_frames1-- > 0) {
			if (pos & ~MASK) {
				pos &= MASK;
				S1 = S2;
				if (src_frames1-- > 0) {
					get_s16_end = &&after_get2;
					goto *get;
				after_get2:
					S2 = sample;
					src += src_step;
				}
			}
			val = S1 + ((S2 - S1) * (signed int)pos) / BITS;
			if (val < -32768)
				val = -32768;
			else if (val > 32767)
				val = 32767;
			sample = val;
			goto *put;
#define PUT_S16_END after_put
#include "plugin_ops.h"
#undef PUT_S16_END
		after_put:
			dst += dst_step;
			pos += data->pitch;
		}
		rvoices->last_S1 = S1;
		rvoices->last_S2 = S2;
		rvoices++;
	}
	data->pos = pos;
}

static void resample_shrink(snd_pcm_plugin_t *plugin,
			    const snd_pcm_plugin_voice_t *src_voices,
			    snd_pcm_plugin_voice_t *dst_voices,
			    int src_frames, int dst_frames)
{
	unsigned int pos = 0;
	signed int val;
	signed short S1, S2;
	char *src, *dst;
	unsigned int voice;
	int src_step, dst_step;
	int src_frames1, dst_frames1;
	rate_t *data = (rate_t *)plugin->extra_data;
	rate_voice_t *rvoices = data->voices;
	
#define GET_S16_LABELS
#define PUT_S16_LABELS
#include "plugin_ops.h"
#undef GET_S16_LABELS
#undef PUT_S16_LABELS
	void *get = get_s16_labels[data->get];
	void *put = put_s16_labels[data->put];
	signed short sample = 0;

	for (voice = 0; voice < plugin->src_format.voices; ++voice) {
		pos = data->pos;
		S1 = rvoices->last_S1;
		S2 = rvoices->last_S2;
		if (!src_voices[voice].enabled) {
			if (dst_voices[voice].wanted)
				snd_pcm_area_silence(&dst_voices[voice].area, 0, dst_frames, plugin->dst_format.format);
			dst_voices[voice].enabled = 0;
			continue;
		}
		dst_voices[voice].enabled = 1;
		src = (char *)src_voices[voice].area.addr + src_voices[voice].area.first / 8;
		dst = (char *)dst_voices[voice].area.addr + dst_voices[voice].area.first / 8;
		src_step = src_voices[voice].area.step / 8;
		dst_step = dst_voices[voice].area.step / 8;
		src_frames1 = src_frames;
		dst_frames1 = dst_frames;
		while (dst_frames1 > 0) {
			S1 = S2;
			if (src_frames1-- > 0) {
				goto *get;
#define GET_S16_END after_get
#include "plugin_ops.h"
#undef GET_S16_END
			after_get:
				S2 = sample;
				src += src_step;
			}
			if (pos & ~MASK) {
				pos &= MASK;
				val = S1 + ((S2 - S1) * (signed int)pos) / BITS;
				if (val < -32768)
					val = -32768;
				else if (val > 32767)
					val = 32767;
				sample = val;
				goto *put;
#define PUT_S16_END after_put
#include "plugin_ops.h"
#undef PUT_S16_END
			after_put:
				dst += dst_step;
				dst_frames1--;
			}
			pos += data->pitch;
		}
		rvoices->last_S1 = S1;
		rvoices->last_S2 = S2;
		rvoices++;
	}
	data->pos = pos;
}

static ssize_t rate_src_frames(snd_pcm_plugin_t *plugin, size_t frames)
{
	rate_t *data;
	ssize_t res;

	if (plugin == NULL || frames <= 0)
		return -EINVAL;
	data = (rate_t *)plugin->extra_data;
	if (plugin->src_format.rate < plugin->dst_format.rate) {
		res = (((frames * data->pitch) + (BITS/2)) >> SHIFT);
	} else {
		res = (((frames << SHIFT) + (data->pitch / 2)) / data->pitch);		
	}
	if (data->old_src_frames > 0) {
		ssize_t frames1 = frames, res1 = data->old_dst_frames;
		while (data->old_src_frames < frames1) {
			frames1 >>= 1;
			res1 <<= 1;
		}
		while (data->old_src_frames > frames1) {
			frames1 <<= 1;
			res1 >>= 1;
		}
		if (data->old_src_frames == frames1)
			return res1;
	}
	data->old_src_frames = frames;
	data->old_dst_frames = res;
	return res;
}

static ssize_t rate_dst_frames(snd_pcm_plugin_t *plugin, size_t frames)
{
	rate_t *data;
	ssize_t res;

	if (plugin == NULL || frames <= 0)
		return -EINVAL;
	data = (rate_t *)plugin->extra_data;
	if (plugin->src_format.rate < plugin->dst_format.rate) {
		res = (((frames << SHIFT) + (data->pitch / 2)) / data->pitch);
	} else {
		res = (((frames * data->pitch) + (BITS/2)) >> SHIFT);
	}
	if (data->old_dst_frames > 0) {
		ssize_t frames1 = frames, res1 = data->old_src_frames;
		while (data->old_dst_frames < frames1) {
			frames1 >>= 1;
			res1 <<= 1;
		}
		while (data->old_dst_frames > frames1) {
			frames1 <<= 1;
			res1 >>= 1;
		}
		if (data->old_dst_frames == frames1)
			return res1;
	}
	data->old_dst_frames = frames;
	data->old_src_frames = res;
	return res;
}

static ssize_t rate_transfer(snd_pcm_plugin_t *plugin,
			     const snd_pcm_plugin_voice_t *src_voices,
			     snd_pcm_plugin_voice_t *dst_voices,
			     size_t frames)
{
	size_t dst_frames;
	unsigned int voice;
	rate_t *data;

	if (plugin == NULL || src_voices == NULL || dst_voices == NULL)
		return -EFAULT;
	if (frames == 0)
		return 0;
	for (voice = 0; voice < plugin->src_format.voices; voice++) {
		if (src_voices[voice].area.first % 8 != 0 || 
		    src_voices[voice].area.step % 8 != 0)
			return -EINVAL;
		if (dst_voices[voice].area.first % 8 != 0 || 
		    dst_voices[voice].area.step % 8 != 0)
			return -EINVAL;
	}

	dst_frames = rate_dst_frames(plugin, frames);
	data = (rate_t *)plugin->extra_data;
	data->func(plugin, src_voices, dst_voices, frames, dst_frames);
	return dst_frames;
}

static int rate_action(snd_pcm_plugin_t *plugin,
		       snd_pcm_plugin_action_t action,
		       unsigned long udata UNUSED)
{
	if (plugin == NULL)
		return -EINVAL;
	switch (action) {
	case INIT:
	case PREPARE:
	case DRAIN:
	case FLUSH:
		rate_init(plugin);
		break;
	default:
		break;
	}
	return 0;	/* silenty ignore other actions */
}

int snd_pcm_plugin_build_rate(snd_pcm_plugin_handle_t *handle,
			      int channel,
			      snd_pcm_format_t *src_format,
			      snd_pcm_format_t *dst_format,
			      snd_pcm_plugin_t **r_plugin)
{
	int err;
	rate_t *data;
	snd_pcm_plugin_t *plugin;

	if (r_plugin == NULL)
		return -EINVAL;
	*r_plugin = NULL;

	if (src_format->voices != dst_format->voices)
		return -EINVAL;
	if (src_format->voices < 1)
		return -EINVAL;
	if (snd_pcm_format_linear(src_format->format) <= 0)
		return -EINVAL;
	if (snd_pcm_format_linear(dst_format->format) <= 0)
		return -EINVAL;
	if (src_format->rate == dst_format->rate)
		return -EINVAL;

	err = snd_pcm_plugin_build(handle, channel,
				   "rate conversion",
				   src_format,
				   dst_format,
				   sizeof(rate_t) + src_format->voices * sizeof(rate_voice_t),
				   &plugin);
	if (err < 0)
		return err;
	data = (rate_t *)plugin->extra_data;
	data->get = getput_index(src_format->format);
	data->put = getput_index(dst_format->format);

	if (src_format->rate < dst_format->rate) {
		data->pitch = ((src_format->rate << SHIFT) + (dst_format->rate >> 1)) / dst_format->rate;
		data->func = resample_expand;
	} else {
		data->pitch = ((dst_format->rate << SHIFT) + (src_format->rate >> 1)) / src_format->rate;
		data->func = resample_shrink;
	}
	data->pos = 0;
	rate_init(plugin);
	data->old_src_frames = data->old_dst_frames = 0;
	plugin->transfer = rate_transfer;
	plugin->src_frames = rate_src_frames;
	plugin->dst_frames = rate_dst_frames;
	plugin->action = rate_action;
	*r_plugin = plugin;
	return 0;
}
