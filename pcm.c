/*
 * Linux driver for Mytek Digital Stereo192-DSD DAC USB2
 *
 * PCM driver
 *
 * Based on 6fire usb driver by Torsten Schenk
 *
 * Adapted for Mytek by	: Jurgen Kramer
 * Last updated		: May 27, 2013
 * Copyright		: (C) Jurgen Kramer
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "pcm.h"
#include "chip.h"
#include "comm.h"
#include "control.h"

enum {
	OUT_N_CHANNELS = 6, IN_N_CHANNELS = 4
};

/* keep next two synced with
 * FW_EP_W_MAX_PACKET_SIZE[] and RATES_MAX_PACKET_SIZE
 * and CONTROL_RATE_XXX in control.h */
static const int rates_in_packet_size[] = { 228, 228, 420, 420, 404, 404 };
static const int rates_out_packet_size[] = { 228, 228, 420, 420, 604, 604 };
static const int rates[] = { 44100, 48000, 88200, 96000, 176400, 192000 };
static const int rates_alsaid[] = {
	SNDRV_PCM_RATE_44100, SNDRV_PCM_RATE_48000,
	SNDRV_PCM_RATE_88200, SNDRV_PCM_RATE_96000,
	SNDRV_PCM_RATE_176400, SNDRV_PCM_RATE_192000 };

enum { /* settings for pcm */
	OUT_EP = 6, IN_EP = 2, MAX_BUFSIZE = 128 * 1024
};

enum { /* pcm streaming states */
	STREAM_DISABLED, /* no pcm streaming */
	STREAM_STARTING, /* pcm streaming requested, waiting to become ready */
	STREAM_RUNNING, /* pcm streaming running */
	STREAM_STOPPING
};

static const struct snd_pcm_hardware pcm_hw = {
	.info = SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_BATCH,

	.formats = SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S32_LE,

	.rates = SNDRV_PCM_RATE_44100 |
		SNDRV_PCM_RATE_48000 |
		SNDRV_PCM_RATE_88200 |
		SNDRV_PCM_RATE_96000 |
		SNDRV_PCM_RATE_176400 |
		SNDRV_PCM_RATE_192000,

	.rate_min = 44100,
	.rate_max = 192000,
	.channels_min = 1,
	.channels_max = 0, /* set in pcm_open, depending on capture/playback */
	.buffer_bytes_max = MAX_BUFSIZE,
	.period_bytes_min = PCM_N_PACKETS_PER_URB * (PCM_MAX_PACKET_SIZE - 4),
	.period_bytes_max = MAX_BUFSIZE,
	.periods_min = 2,
	.periods_max = 1024
};

static int mytek_pcm_set_rate(struct pcm_runtime *rt)
{
	int ret;
	struct control_runtime *ctrl_rt = rt->chip->control;

	ctrl_rt->usb_streaming = false;
	ret = ctrl_rt->update_streaming(ctrl_rt);
	if (ret < 0) {
		snd_printk(KERN_ERR PREFIX "error stopping streaming while "
				"setting samplerate %d.\n", rates[rt->rate]);
		return ret;
	}

	ret = ctrl_rt->set_rate(ctrl_rt, rt->rate);
	if (ret < 0) {
		snd_printk(KERN_ERR PREFIX "error setting samplerate %d.\n",
				rates[rt->rate]);
		return ret;
	}

	ret = ctrl_rt->set_channels(ctrl_rt, OUT_N_CHANNELS, IN_N_CHANNELS,
			false, false);
	if (ret < 0) {
		snd_printk(KERN_ERR PREFIX "error initializing channels "
				"while setting samplerate %d.\n",
				rates[rt->rate]);
		return ret;
	}

	ctrl_rt->usb_streaming = true;
	ret = ctrl_rt->update_streaming(ctrl_rt);
	if (ret < 0) {
		snd_printk(KERN_ERR PREFIX "error starting streaming while "
				"setting samplerate %d.\n", rates[rt->rate]);
		return ret;
	}

	rt->in_n_analog = IN_N_CHANNELS;
	rt->out_n_analog = OUT_N_CHANNELS;
	rt->in_packet_size = rates_in_packet_size[rt->rate];
	rt->out_packet_size = rates_out_packet_size[rt->rate];
	return 0;
}

static struct pcm_substream *mytek_pcm_get_substream(
		struct snd_pcm_substream *alsa_sub)
{
	struct pcm_runtime *rt = snd_pcm_substream_chip(alsa_sub);

	if (alsa_sub->stream == SNDRV_PCM_STREAM_PLAYBACK)
		return &rt->playback;

	snd_printk(KERN_ERR PREFIX "error getting pcm substream slot.\n");
	return NULL;
}

/* call with stream_mutex locked */
static void mytek_pcm_stream_stop(struct pcm_runtime *rt)
{
	int i;
	struct control_runtime *ctrl_rt = rt->chip->control;

	if (rt->stream_state != STREAM_DISABLED) {

		rt->stream_state = STREAM_STOPPING;

		for (i = 0; i < PCM_N_URBS; i++) {
			usb_kill_urb(&rt->in_urbs[i].instance);
			usb_kill_urb(&rt->out_urbs[i].instance);
		}

		ctrl_rt->usb_streaming = false;
		ctrl_rt->update_streaming(ctrl_rt);
		rt->stream_state = STREAM_DISABLED;
	}
}

/* call with stream_mutex locked */
static int mytek_pcm_stream_start(struct pcm_runtime *rt)
{
	int ret;
	int i;
	int k;
	struct usb_iso_packet_descriptor *packet;

	if (rt->stream_state == STREAM_DISABLED) {
		/* submit our in urbs */
		rt->stream_wait_cond = false;
		rt->stream_state = STREAM_STARTING;
		for (i = 0; i < PCM_N_URBS; i++) {
			for (k = 0; k < PCM_N_PACKETS_PER_URB; k++) {
				packet = &rt->in_urbs[i].packets[k];
				packet->offset = k * rt->in_packet_size;
				packet->length = rt->in_packet_size;
				packet->actual_length = 0;
				packet->status = 0;
			}
			ret = usb_submit_urb(&rt->in_urbs[i].instance,
					GFP_ATOMIC);
			if (ret) {
				mytek_pcm_stream_stop(rt);
				return ret;
			}
		}

		/* wait for first out urb to return (sent in in urb handler) */
		wait_event_timeout(rt->stream_wait_queue, rt->stream_wait_cond,
				HZ);
		if (rt->stream_wait_cond)
			rt->stream_state = STREAM_RUNNING;
		else {
			mytek_pcm_stream_stop(rt);
			return -EIO;
		}
	}
	return 0;
}

/* call with substream locked */
static void mytek_pcm_playback(struct pcm_substream *sub,
		struct pcm_urb *urb)
{
	int i;
	int frame;
	int frame_count;
	struct pcm_runtime *rt = snd_pcm_substream_chip(sub->instance);
	struct snd_pcm_runtime *alsa_rt = sub->instance->runtime;
	u32 *src = (u32 *) (alsa_rt->dma_area + sub->dma_off
			* (alsa_rt->frame_bits >> 3));
	u32 *src_end = (u32 *) (alsa_rt->dma_area + alsa_rt->buffer_size
			* (alsa_rt->frame_bits >> 3));
	u32 *dest;
	int bytes_per_frame = alsa_rt->channels << 2;

	if (alsa_rt->format == SNDRV_PCM_FORMAT_S32_LE)
		dest = (u32 *) (urb->buffer - 1);
	else if (alsa_rt->format == SNDRV_PCM_FORMAT_S24_LE)
		dest = (u32 *) (urb->buffer);
	else {
		snd_printk(KERN_ERR PREFIX "Unknown sample format.");
		return;
	}

	for (i = 0; i < PCM_N_PACKETS_PER_URB; i++) {
		/* at least 4 header bytes for valid packet.
		 * after that: 32 bits per sample for analog channels */
		if (urb->packets[i].length > 4)
			frame_count = (urb->packets[i].length - 4)
					/ (rt->out_n_analog << 2);
		else
			frame_count = 0;
		dest++; /* skip leading 4 bytes of every frame */
		for (frame = 0; frame < frame_count; frame++) {
			memcpy(dest, src, bytes_per_frame);
			src += alsa_rt->channels;
			dest += rt->out_n_analog;
			sub->dma_off++;
			sub->period_off++;
			if (src == src_end) {
				src = (u32 *) alsa_rt->dma_area;
				sub->dma_off = 0;
			}
		}
	}
}

static void mytek_pcm_in_urb_handler(struct urb *usb_urb)
{
	struct pcm_urb *in_urb = usb_urb->context;
	struct pcm_urb *out_urb = in_urb->peer;
	struct pcm_runtime *rt = in_urb->chip->pcm;
	struct pcm_substream *sub;
	unsigned long flags;
	int total_length = 0;
	int frame_count;
	int frame;
	int channel;
	int i;
	u8 *dest;

	if (usb_urb->status || rt->panic || rt->stream_state == STREAM_STOPPING)
		return;
	for (i = 0; i < PCM_N_PACKETS_PER_URB; i++)
		if (in_urb->packets[i].status) {
			rt->panic = true;
			return;
		}

	if (rt->stream_state == STREAM_DISABLED) {
		snd_printk(KERN_ERR PREFIX "internal error: "
				"stream disabled in in-urb handler.\n");
		return;
	}

	/* setup out urb structure */
	for (i = 0; i < PCM_N_PACKETS_PER_URB; i++) {
		out_urb->packets[i].offset = total_length;
		out_urb->packets[i].length = (in_urb->packets[i].actual_length
				- 4) / (rt->in_n_analog << 2)
				* (rt->out_n_analog << 2) + 4;
		out_urb->packets[i].status = 0;
		total_length += out_urb->packets[i].length;
	}
	memset(out_urb->buffer, 0, total_length);

	/* now send our playback data (if a free out urb was found) */
	sub = &rt->playback;
	spin_lock_irqsave(&sub->lock, flags);
	if (sub->active) {
		mytek_pcm_playback(sub, out_urb);
		if (sub->period_off >= sub->instance->runtime->period_size) {
			sub->period_off %= sub->instance->runtime->period_size;
			spin_unlock_irqrestore(&sub->lock, flags);
			snd_pcm_period_elapsed(sub->instance);
		} else
			spin_unlock_irqrestore(&sub->lock, flags);
	} else
		spin_unlock_irqrestore(&sub->lock, flags);

	/* setup the 4th byte of each sample (0x40 for analog channels) */
	dest = out_urb->buffer;
	for (i = 0; i < PCM_N_PACKETS_PER_URB; i++)
		if (out_urb->packets[i].length >= 4) {
			frame_count = (out_urb->packets[i].length - 4)
					/ (rt->out_n_analog << 2);
			*(dest++) = 0xaa;
			*(dest++) = 0xaa;
			*(dest++) = frame_count;
			*(dest++) = 0x00;
			for (frame = 0; frame < frame_count; frame++)
				for (channel = 0;
						channel < rt->out_n_analog;
						channel++) {
					dest += 3; /* skip sample data */
					*(dest++) = 0x40;
				}
		}
	usb_submit_urb(&out_urb->instance, GFP_ATOMIC);
	usb_submit_urb(&in_urb->instance, GFP_ATOMIC);
}

static void mytek_pcm_out_urb_handler(struct urb *usb_urb)
{
	struct pcm_urb *urb = usb_urb->context;
	struct pcm_runtime *rt = urb->chip->pcm;

	if (rt->stream_state == STREAM_STARTING) {
		rt->stream_wait_cond = true;
		wake_up(&rt->stream_wait_queue);
	}
}

static int mytek_pcm_open(struct snd_pcm_substream *alsa_sub)
{
	struct pcm_runtime *rt = snd_pcm_substream_chip(alsa_sub);
	struct pcm_substream *sub = NULL;
	struct snd_pcm_runtime *alsa_rt = alsa_sub->runtime;

	if (rt->panic)
		return -EPIPE;

	mutex_lock(&rt->stream_mutex);
	alsa_rt->hw = pcm_hw;

	if (alsa_sub->stream == SNDRV_PCM_STREAM_PLAYBACK) {

		if (rt->rate < ARRAY_SIZE(rates))
			alsa_rt->hw.rates = rates_alsaid[rt->rate];
		alsa_rt->hw.channels_max = OUT_N_CHANNELS;
		sub = &rt->playback;
	}

	if (!sub) {
		mutex_unlock(&rt->stream_mutex);
		snd_printk(KERN_ERR PREFIX "invalid stream type.\n");
		return -EINVAL;
	}

	sub->instance = alsa_sub;
	sub->active = false;
	mutex_unlock(&rt->stream_mutex);

	return 0;
}

static int mytek_pcm_close(struct snd_pcm_substream *alsa_sub)
{
	struct pcm_runtime *rt = snd_pcm_substream_chip(alsa_sub);
	struct pcm_substream *sub = mytek_pcm_get_substream(alsa_sub);
	unsigned long flags;

	if (rt->panic)
			return 0;

	mutex_lock(&rt->stream_mutex);
	if (sub) {
		/* deactivate substream */
		spin_lock_irqsave(&sub->lock, flags);
		sub->instance = NULL;
		sub->active = false;
		spin_unlock_irqrestore(&sub->lock, flags);

		/* all substreams closed? if so, stop streaming */
		if (!rt->playback.instance) {
			mytek_pcm_stream_stop(rt);
			rt->rate = ARRAY_SIZE(rates);
		}
	}
	mutex_unlock(&rt->stream_mutex);

	return 0;
}

static int mytek_pcm_hw_params(struct snd_pcm_substream *alsa_sub,
		struct snd_pcm_hw_params *hw_params)
{
	return snd_pcm_lib_malloc_pages(alsa_sub,
			params_buffer_bytes(hw_params));
}

static int mytek_pcm_hw_free(struct snd_pcm_substream *alsa_sub)
{
	return snd_pcm_lib_free_pages(alsa_sub);
}

static int mytek_pcm_prepare(struct snd_pcm_substream *alsa_sub)
{
	struct pcm_runtime *rt = snd_pcm_substream_chip(alsa_sub);
	struct pcm_substream *sub = mytek_pcm_get_substream(alsa_sub);
	struct snd_pcm_runtime *alsa_rt = alsa_sub->runtime;
	int ret;

	if (rt->panic)
		return -EPIPE;
	if (!sub)
		return -ENODEV;

	mutex_lock(&rt->stream_mutex);
	sub->dma_off = 0;
	sub->period_off = 0;

	if (rt->stream_state == STREAM_DISABLED) {
		for (rt->rate = 0; rt->rate < ARRAY_SIZE(rates); rt->rate++)
			if (alsa_rt->rate == rates[rt->rate])
				break;
		if (rt->rate == ARRAY_SIZE(rates)) {
			mutex_unlock(&rt->stream_mutex);
			snd_printk("invalid rate %d in prepare.\n",
					alsa_rt->rate);
			return -EINVAL;
		}

		ret = mytek_pcm_set_rate(rt);
		if (ret) {
			mutex_unlock(&rt->stream_mutex);
			return ret;
		}
		ret = mytek_pcm_stream_start(rt);
		if (ret) {
			mutex_unlock(&rt->stream_mutex);
			snd_printk(KERN_ERR PREFIX
					"could not start pcm stream.\n");
			return ret;
		}
	}
	mutex_unlock(&rt->stream_mutex);
	
	return 0;
}

static int mytek_pcm_trigger(struct snd_pcm_substream *alsa_sub, int cmd)
{
	struct pcm_substream *sub = mytek_pcm_get_substream(alsa_sub);
	struct pcm_runtime *rt = snd_pcm_substream_chip(alsa_sub);
	unsigned long flags;

	if (rt->panic)
		return -EPIPE;
	if (!sub)
		return -ENODEV;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		spin_lock_irqsave(&sub->lock, flags);
		sub->active = true;
		spin_unlock_irqrestore(&sub->lock, flags);
		return 0;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		spin_lock_irqsave(&sub->lock, flags);
		sub->active = false;
		spin_unlock_irqrestore(&sub->lock, flags);
		return 0;

	default:
		return -EINVAL;
	}
}

static snd_pcm_uframes_t mytek_pcm_pointer(
		struct snd_pcm_substream *alsa_sub)
{
	struct pcm_substream *sub = mytek_pcm_get_substream(alsa_sub);
	struct pcm_runtime *rt = snd_pcm_substream_chip(alsa_sub);
	unsigned long flags;
	snd_pcm_uframes_t ret;

	if (rt->panic || !sub)
		return SNDRV_PCM_STATE_XRUN;

	spin_lock_irqsave(&sub->lock, flags);
	ret = sub->dma_off;
	spin_unlock_irqrestore(&sub->lock, flags);

	return ret;
}

static struct snd_pcm_ops pcm_ops = {
	.open = mytek_pcm_open,
	.close = mytek_pcm_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_params = mytek_pcm_hw_params,
	.hw_free = mytek_pcm_hw_free,
	.prepare = mytek_pcm_prepare,
	.trigger = mytek_pcm_trigger,
	.pointer = mytek_pcm_pointer,
};

static void mytek_pcm_init_urb(struct pcm_urb *urb,
		struct sfire_chip *chip, bool in, int ep,
		void (*handler)(struct urb *))
{
	urb->chip = chip;
	usb_init_urb(&urb->instance);
	urb->instance.transfer_buffer = urb->buffer;
	urb->instance.transfer_buffer_length =
			PCM_N_PACKETS_PER_URB * PCM_MAX_PACKET_SIZE;
	urb->instance.dev = chip->dev;
	urb->instance.pipe = in ? usb_rcvisocpipe(chip->dev, ep)
			: usb_sndisocpipe(chip->dev, ep);
	urb->instance.interval = 1;
	urb->instance.transfer_flags = URB_ISO_ASAP;
	urb->instance.complete = handler;
	urb->instance.context = urb;
	urb->instance.number_of_packets = PCM_N_PACKETS_PER_URB;
}

int mytek_pcm_init(struct sfire_chip *chip)
{
	int i;
	int ret;
	struct snd_pcm *pcm;
	struct pcm_runtime *rt =
			kzalloc(sizeof(struct pcm_runtime), GFP_KERNEL);

	if (!rt)
		return -ENOMEM;

	rt->chip = chip;
	rt->stream_state = STREAM_DISABLED;
	rt->rate = ARRAY_SIZE(rates);
	init_waitqueue_head(&rt->stream_wait_queue);
	mutex_init(&rt->stream_mutex);

	spin_lock_init(&rt->playback.lock);

	for (i = 0; i < PCM_N_URBS; i++) {
		mytek_pcm_init_urb(&rt->in_urbs[i], chip, true, IN_EP,
				mytek_pcm_in_urb_handler);
		mytek_pcm_init_urb(&rt->out_urbs[i], chip, false, OUT_EP,
				mytek_pcm_out_urb_handler);

		rt->in_urbs[i].peer = &rt->out_urbs[i];
		rt->out_urbs[i].peer = &rt->in_urbs[i];
	}

	ret = snd_pcm_new(chip->card, "MytekUSB2", 0, 1, 0, &pcm);

	if (ret < 0) {
		kfree(rt);
		snd_printk(KERN_ERR PREFIX "cannot create pcm instance.\n");
		return ret;
	}

	pcm->private_data = rt;
	strcpy(pcm->name, "Mytek USB2");
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &pcm_ops);

	ret = snd_pcm_lib_preallocate_pages_for_all(pcm,
			SNDRV_DMA_TYPE_CONTINUOUS,
			snd_dma_continuous_data(GFP_KERNEL),
			MAX_BUFSIZE, MAX_BUFSIZE);
	if (ret) {
		kfree(rt);
		snd_printk(KERN_ERR PREFIX
				"error preallocating pcm buffers.\n");
		return ret;
	}
	rt->instance = pcm;
	chip->pcm = rt;

	return 0;
}

void mytek_pcm_abort(struct sfire_chip *chip)
{
	struct pcm_runtime *rt = chip->pcm;
	int i;

	if (rt) {
		rt->panic = true;

		if (rt->playback.instance)
			snd_pcm_stop(rt->playback.instance,
					SNDRV_PCM_STATE_XRUN);

		for (i = 0; i < PCM_N_URBS; i++) {
			usb_poison_urb(&rt->in_urbs[i].instance);
			usb_poison_urb(&rt->out_urbs[i].instance);
		}

	}
}

void mytek_pcm_destroy(struct sfire_chip *chip)
{
	kfree(chip->pcm);
	chip->pcm = NULL;
}
