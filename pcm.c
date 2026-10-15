/*
 * Linux driver for Mytek Digital Stereo192-DSD DAC USB2
 *
 * PCM driver
 *
 * Based on 6fire usb driver
 *
 * Adapted for Mytek by	: Jurgen Kramer
 * Last updated		: Oct 15, 2026
 * Copyright		: (C) Jurgen Kramer
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/bitops.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <sound/info.h>

#include "pcm.h"
#include "chip.h"
#include "comm.h"
#include "control.h"

/*
 * The device paces playback through its iso IN endpoint (implicit feedback):
 * for every IN packet received, an OUT packet carrying the same number of
 * frames is sent. Driver <= 1.x only sent an OUT urb when the matching IN
 * urb completed, so the host controller had about one urb (1ms) of playback
 * data queued. Any completion latency above that leaves the OUT ring empty;
 * xhci then restarts the stream discontiguously and the DAC hears a gap.
 * We queue out_lead silent urbs first so the feedback-driven urbs are
 * always that far ahead of the bus.
 */
static unsigned int out_lead = 6;
module_param(out_lead, uint, 0644);
MODULE_PARM_DESC(out_lead,
	"Silent 1ms output urbs queued ahead at stream start, i.e. how much host latency is tolerated (0 = driver 1.x behaviour, max 12, default 6).");

/*
 * What to send for an IN packet that carries no feedback (zero length or
 * error status). Driver 1.x ("usbworkaround") always pretended it held
 * 5 frames, which is only close to correct at 44.1kHz.
 */
static unsigned int zlp_fill = 1;
module_param(zlp_fill, uint, 0644);
MODULE_PARM_DESC(zlp_fill,
	"Frames sent for an IN packet without feedback: 0 = none, 1 = nominal for the sample rate (default), 2 = always 5 (driver 1.x).");

enum {
	OUT_N_CHANNELS = 6, IN_N_CHANNELS = 4
};

enum {
	OUT_LEAD_MAX = PCM_N_URBS - 4
};

enum { /* zlp_fill values */
	ZLP_FILL_NONE, ZLP_FILL_NOMINAL, ZLP_FILL_LEGACY
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
	OUT_EP = 6, IN_EP = 2, MAX_BUFSIZE = 512 * 1024
};

enum { /* pcm streaming states */
	STREAM_DISABLED, /* no pcm streaming */
	STREAM_STARTING, /* pcm streaming requested, waiting to become ready */
	STREAM_RUNNING, /* pcm streaming running */
	STREAM_STOPPING
};

static const char * const stream_state_names[] = {
	"disabled", "starting", "running", "stopping"
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
	/* stereo DAC: exposing 1-6 channels made sound servers pick mono */
	.channels_min = 2,
	.channels_max = 2,
	.buffer_bytes_max = MAX_BUFSIZE,
	.period_bytes_min = PCM_N_PACKETS_PER_URB * (PCM_MAX_PACKET_SIZE - 4),
	.period_bytes_max = MAX_BUFSIZE,
	.periods_min = 2,
	.periods_max = 1024
};

/*
 * Read packet sizes and interval of the iso endpoints from the active
 * altsetting of interface 1, falling back to the table values.
 */
static void mytek_pcm_setup_endpoints(struct pcm_runtime *rt)
{
	struct usb_device *dev = rt->chip->dev;
	struct usb_interface *iface = usb_ifnum_to_if(dev, 1);
	struct usb_host_interface *alts = iface ? iface->cur_altsetting : NULL;
	int in_maxp = 0, out_maxp = 0;
	int interval = 0;
	int i;

	for (i = 0; alts && i < alts->desc.bNumEndpoints; i++) {
		const struct usb_endpoint_descriptor *epd =
			&alts->endpoint[i].desc;
		int maxp;

		if (!usb_endpoint_xfer_isoc(epd))
			continue;
		maxp = usb_endpoint_maxp(epd) * usb_endpoint_maxp_mult(epd);
		if (usb_endpoint_num(epd) == IN_EP && usb_endpoint_dir_in(epd)) {
			in_maxp = maxp;
		} else if (usb_endpoint_num(epd) == OUT_EP &&
			   usb_endpoint_dir_out(epd)) {
			out_maxp = maxp;
			interval = epd->bInterval;
		}
	}

	rt->in_packet_size = rates_in_packet_size[rt->rate];
	rt->out_packet_size = rates_out_packet_size[rt->rate];
	if (in_maxp > 4 && in_maxp <= PCM_MAX_PACKET_SIZE)
		rt->in_packet_size = in_maxp;
	if (out_maxp > 4 && out_maxp <= PCM_MAX_PACKET_SIZE)
		rt->out_packet_size = out_maxp;
	rt->out_max_frames = (rt->out_packet_size - 4) / (rt->out_n_analog << 2);

	/* bInterval is 2^(n-1) (micro)frames for both full and high speed */
	interval = clamp(interval, 1, 4);
	rt->urb_interval = 1 << (interval - 1);
	rt->packets_per_sec = (dev->speed >= USB_SPEED_HIGH ? 8000 : 1000)
			/ rt->urb_interval;

	dev_dbg(&dev->dev,
		 "rate %d: alt %d, IN maxp %d (table %d), OUT maxp %d (table %d), %u packets/s\n",
		 rates[rt->rate], alts ? alts->desc.bAlternateSetting : -1,
		 in_maxp, rates_in_packet_size[rt->rate],
		 out_maxp, rates_out_packet_size[rt->rate],
		 rt->packets_per_sec);
}

static int mytek_pcm_set_rate(struct pcm_runtime *rt)
{
	int ret;
	struct control_runtime *ctrl_rt = rt->chip->control;

	ctrl_rt->usb_streaming = false;
	ret = ctrl_rt->update_streaming(ctrl_rt);
	if (ret < 0) {
		dev_err(&rt->chip->dev->dev,
			"error stopping streaming while setting samplerate %d.\n",
			rates[rt->rate]);
		return ret;
	}

	ret = ctrl_rt->set_rate(ctrl_rt, rt->rate);
	if (ret < 0) {
		dev_err(&rt->chip->dev->dev,
			"error setting samplerate %d.\n",
			rates[rt->rate]);
		return ret;
	}

	ret = ctrl_rt->set_channels(ctrl_rt, OUT_N_CHANNELS, IN_N_CHANNELS,
			false, false);
	if (ret < 0) {
		dev_err(&rt->chip->dev->dev,
				"error initializing channels while setting samplerate %d.\n",
				rates[rt->rate]);
		return ret;
	}

	ctrl_rt->usb_streaming = true;
	ret = ctrl_rt->update_streaming(ctrl_rt);
	if (ret < 0) {
		dev_err(&rt->chip->dev->dev,
			"error starting streaming while setting samplerate %d.\n",
			rates[rt->rate]);
		return ret;
	}

	rt->in_n_analog = IN_N_CHANNELS;
	rt->out_n_analog = OUT_N_CHANNELS;
	mytek_pcm_setup_endpoints(rt);
	return 0;
}

static struct pcm_substream *mytek_pcm_get_substream(
		struct snd_pcm_substream *alsa_sub)
{
	struct pcm_runtime *rt = snd_pcm_substream_chip(alsa_sub);

	if (alsa_sub->stream == SNDRV_PCM_STREAM_PLAYBACK)
		return &rt->playback;

	dev_err(&rt->chip->dev->dev, "error getting pcm substream slot.\n");
	return NULL;
}

static void mytek_pcm_reset_stats(struct pcm_runtime *rt)
{
	memset(&rt->stats, 0, sizeof(rt->stats));
	rt->stats.in_frames_min = U32_MAX;
	rt->stats.started = ktime_get();
}

/*
 * One summary line per stream: in the normal log only if something went
 * wrong or the host came close to the out_lead headroom, else debug only.
 */
static void mytek_pcm_log_stats(struct pcm_runtime *rt)
{
	struct pcm_stats *st = &rt->stats;
	u32 warn_gap_us = max_t(u32, rt->out_lead * 500, 1500);
	bool notable = st->in_empty || st->in_pkt_err || st->in_urb_err ||
		st->in_submit_err ||
		st->in_clamped || st->out_pkt_err || st->out_urb_err ||
		st->out_submit_err || st->out_starved ||
		st->in_gap_max_us > warn_gap_us;

	if (!st->in_urbs)
		return;
	if (!notable) {
		dev_dbg(&rt->chip->dev->dev,
			"stream stats: %llds @%d, no problems, gap_max=%uus\n",
			ktime_ms_delta(ktime_get(), st->started) / 1000,
			rates[rt->rate], st->in_gap_max_us);
		return;
	}
	dev_info(&rt->chip->dev->dev,
		 "stream stats: %llds @%d lead=%u zlp_fill=%u | in: urbs=%llu empty=%llu pkt_err=%llu urb_err=%llu submit_err=%llu clamped=%llu late=%llu gap_max=%uus frames=%u..%u | out: urbs=%llu pkt_err=%llu urb_err=%llu submit_err=%llu starved=%llu\n",
		 ktime_ms_delta(ktime_get(), st->started) / 1000,
		 rates[rt->rate], rt->out_lead, rt->zlp_fill,
		 st->in_urbs, st->in_empty, st->in_pkt_err, st->in_urb_err,
		 st->in_submit_err, st->in_clamped, st->in_late,
		 st->in_gap_max_us,
		 st->in_frames_min == U32_MAX ? 0 : st->in_frames_min,
		 st->in_frames_max,
		 st->out_urbs, st->out_pkt_err, st->out_urb_err,
		 st->out_submit_err, st->out_starved);
}

/* call with stream_mutex locked */
static void mytek_pcm_stream_stop(struct pcm_runtime *rt)
{
	int i;
	struct control_runtime *ctrl_rt = rt->chip->control;

	if (rt->stream_state != STREAM_DISABLED) {

		rt->stream_state = STREAM_STOPPING;

		for (i = 0; i < PCM_N_URBS; i++) {
			usb_kill_urb(rt->in_urbs[i].instance);
			usb_kill_urb(rt->out_urbs[i].instance);
		}

		ctrl_rt->usb_streaming = false;
		ctrl_rt->update_streaming(ctrl_rt);
		rt->stream_state = STREAM_DISABLED;
		mytek_pcm_log_stats(rt);
	}
}

/* frames per packet the device consumes at the current rate on average */
static unsigned int mytek_pcm_nominal_frames(struct pcm_runtime *rt)
{
	unsigned int frames;

	rt->nominal_acc += rates[rt->rate];
	frames = rt->nominal_acc / rt->packets_per_sec;
	rt->nominal_acc %= rt->packets_per_sec;
	return frames;
}

static unsigned int mytek_pcm_fallback_frames(struct pcm_runtime *rt)
{
	switch (rt->zlp_fill) {
	case ZLP_FILL_NONE:
		return 0;
	case ZLP_FILL_LEGACY:
		return 5;
	default:
		return mytek_pcm_nominal_frames(rt);
	}
}

static struct pcm_urb *mytek_pcm_get_idle_out_urb(struct pcm_runtime *rt)
{
	int i;
	int idx;

	for (i = 0; i < PCM_N_URBS; i++) {
		idx = (rt->out_next + i) % PCM_N_URBS;
		if (test_and_clear_bit(idx, &rt->out_idle)) {
			rt->out_next = (idx + 1) % PCM_N_URBS;
			return &rt->out_urbs[idx];
		}
	}
	return NULL;
}

/* call with substream locked */
static void mytek_pcm_playback(struct pcm_substream *sub,
		struct pcm_urb *urb)
{
	struct pcm_runtime *rt = snd_pcm_substream_chip(sub->instance);
	struct snd_pcm_runtime *alsa_rt = sub->instance->runtime;
	struct usb_iso_packet_descriptor *isoc;
	const __le32 *src = (const __le32 *)(alsa_rt->dma_area
			+ frames_to_bytes(alsa_rt, sub->dma_off));
	unsigned int channels = alsa_rt->channels;
	/* the device takes 24 bits per sample in the 3 low bytes */
	int shift;
	unsigned int frame_count;
	unsigned int frame;
	unsigned int ch;
	int i;
	u8 *dest;
	u32 v;

	if (alsa_rt->format == SNDRV_PCM_FORMAT_S32_LE)
		shift = 8;
	else if (alsa_rt->format == SNDRV_PCM_FORMAT_S24_LE)
		shift = 0;
	else {
		dev_err_ratelimited(&rt->chip->dev->dev,
				    "Unknown sample format.\n");
		return;
	}
	if (channels > rt->out_n_analog)
		channels = rt->out_n_analog;

	for (i = 0; i < PCM_N_PACKETS_PER_URB; i++) {
		isoc = &urb->instance->iso_frame_desc[i];
		frame_count = (isoc->length - 4) / (rt->out_n_analog << 2);
		dest = urb->buffer + isoc->offset + 4;
		for (frame = 0; frame < frame_count; frame++) {
			for (ch = 0; ch < channels; ch++) {
				v = le32_to_cpu(src[ch]) >> shift;
				dest[(ch << 2) + 0] = v;
				dest[(ch << 2) + 1] = v >> 8;
				dest[(ch << 2) + 2] = v >> 16;
			}
			dest += rt->out_n_analog << 2;
			src += alsa_rt->channels;
			sub->dma_off++;
			sub->period_off++;
			if (sub->dma_off >= alsa_rt->buffer_size) {
				src = (const __le32 *)alsa_rt->dma_area;
				sub->dma_off = 0;
			}
		}
	}
}

/*
 * Lay out an out urb carrying frames[i] frames in packet i, filled with
 * playback data if the substream runs and silence otherwise.
 * Returns the alsa substream if a period elapsed.
 */
static struct snd_pcm_substream *mytek_pcm_fill_out_urb(
		struct pcm_runtime *rt, struct pcm_urb *out_urb,
		const unsigned int *frames)
{
	struct pcm_substream *sub = &rt->playback;
	struct snd_pcm_substream *elapsed = NULL;
	struct usb_iso_packet_descriptor *isoc;
	unsigned long flags;
	unsigned int total_length = 0;
	unsigned int frame;
	unsigned int channel;
	int i;
	u8 *dest;

	for (i = 0; i < PCM_N_PACKETS_PER_URB; i++) {
		isoc = &out_urb->instance->iso_frame_desc[i];
		isoc->offset = total_length;
		isoc->length = frames[i] * (rt->out_n_analog << 2) + 4;
		isoc->actual_length = 0;
		isoc->status = 0;
		total_length += isoc->length;
		rt->stats.out_frames += frames[i];
	}
	memset(out_urb->buffer, 0, total_length);

	/* packet header and the 4th byte of each sample (0x40 for analog) */
	dest = out_urb->buffer;
	for (i = 0; i < PCM_N_PACKETS_PER_URB; i++) {
		*(dest++) = 0xaa;
		*(dest++) = 0xaa;
		*(dest++) = frames[i];
		*(dest++) = 0x00;
		for (frame = 0; frame < frames[i]; frame++)
			for (channel = 0; channel < rt->out_n_analog;
					channel++) {
				dest += 3; /* skip sample data */
				*(dest++) = 0x40;
			}
	}

	spin_lock_irqsave(&sub->lock, flags);
	if (sub->active && sub->instance) {
		mytek_pcm_playback(sub, out_urb);
		if (sub->period_off >= sub->instance->runtime->period_size) {
			sub->period_off %= sub->instance->runtime->period_size;
			elapsed = sub->instance;
		}
	}
	spin_unlock_irqrestore(&sub->lock, flags);

	return elapsed;
}

/* call with stream_mutex locked */
static int mytek_pcm_stream_start(struct pcm_runtime *rt)
{
	unsigned int frames[PCM_N_PACKETS_PER_URB];
	struct pcm_urb *out_urb;
	int ret;
	int i;
	int k;
	struct usb_iso_packet_descriptor *packet;

	if (rt->stream_state != STREAM_DISABLED)
		return 0;

	mytek_pcm_reset_stats(rt);
	rt->out_idle = (1UL << PCM_N_URBS) - 1;
	rt->out_next = 0;
	rt->nominal_acc = 0;
	rt->zlp_fill = READ_ONCE(zlp_fill);
	rt->out_lead = min_t(unsigned int, READ_ONCE(out_lead), OUT_LEAD_MAX);
	rt->stream_wait_cond = false;
	rt->stream_state = STREAM_STARTING;

	for (i = 0; i < PCM_N_URBS; i++) {
		rt->in_urbs[i].instance->interval = rt->urb_interval;
		rt->out_urbs[i].instance->interval = rt->urb_interval;
	}

	/* queue silence so playback data always has out_lead ms headroom */
	for (i = 0; i < rt->out_lead; i++) {
		out_urb = mytek_pcm_get_idle_out_urb(rt);
		for (k = 0; k < PCM_N_PACKETS_PER_URB; k++)
			frames[k] = min(mytek_pcm_nominal_frames(rt),
					rt->out_max_frames);
		mytek_pcm_fill_out_urb(rt, out_urb, frames);
		ret = usb_submit_urb(out_urb->instance, GFP_KERNEL);
		if (ret) {
			set_bit(out_urb->index, &rt->out_idle);
			dev_err(&rt->chip->dev->dev,
				"cannot submit out urb: %d\n", ret);
			mytek_pcm_stream_stop(rt);
			return ret;
		}
	}

	/* submit our in urbs */
	for (i = 0; i < PCM_N_URBS; i++) {
		for (k = 0; k < PCM_N_PACKETS_PER_URB; k++) {
			packet = &rt->in_urbs[i].instance->iso_frame_desc[k];
			packet->offset = k * rt->in_packet_size;
			packet->length = rt->in_packet_size;
			packet->actual_length = 0;
			packet->status = 0;
		}
		ret = usb_submit_urb(rt->in_urbs[i].instance, GFP_KERNEL);
		if (ret) {
			dev_err(&rt->chip->dev->dev,
				"cannot submit in urb: %d\n", ret);
			mytek_pcm_stream_stop(rt);
			return ret;
		}
	}

	/* wait for the first in urb to be handled */
	wait_event_timeout(rt->stream_wait_queue, rt->stream_wait_cond, HZ);
	if (rt->stream_wait_cond)
		rt->stream_state = STREAM_RUNNING;
	else {
		dev_err(&rt->chip->dev->dev,
			"device does not send feedback data.\n");
		mytek_pcm_stream_stop(rt);
		return -EIO;
	}
	return 0;
}

static bool mytek_urb_killed(int status)
{
	switch (status) {
	case -ENOENT:		/* killed */
	case -ECONNRESET:	/* unlinked */
	case -ESHUTDOWN:	/* device gone */
	case -ENODEV:
	case -EPERM:		/* poisoned */
		return true;
	default:
		return false;
	}
}

static void mytek_pcm_in_urb_handler(struct urb *usb_urb)
{
	struct pcm_urb *in_urb = usb_urb->context;
	struct pcm_runtime *rt = in_urb->chip->pcm;
	struct pcm_stats *st = &rt->stats;
	struct device *dev = &rt->chip->dev->dev;
	struct usb_iso_packet_descriptor *isoc;
	struct snd_pcm_substream *elapsed = NULL;
	unsigned int frames[PCM_N_PACKETS_PER_URB];
	struct pcm_urb *out_urb;
	unsigned int urb_us;
	unsigned int n;
	ktime_t now;
	s64 gap;
	int ret;
	int i;

	if (mytek_urb_killed(usb_urb->status))
		return;
	if (rt->panic || rt->stream_state == STREAM_STOPPING ||
			rt->stream_state == STREAM_DISABLED)
		return;

	if (usb_urb->status) {
		st->in_urb_err++;
		dev_warn_ratelimited(dev, "in urb error %d\n", usb_urb->status);
	}

	now = ktime_get();
	if (st->in_urbs) {
		urb_us = PCM_N_PACKETS_PER_URB * USEC_PER_SEC
				/ rt->packets_per_sec;
		gap = ktime_us_delta(now, st->last_in);
		if (gap > st->in_gap_max_us)
			st->in_gap_max_us = min_t(s64, gap, U32_MAX);
		if (gap > 2 * urb_us)
			st->in_late++;
	}
	st->last_in = now;
	st->in_urbs++;

	/* the device tells us how many frames to send in each packet */
	for (i = 0; i < PCM_N_PACKETS_PER_URB; i++) {
		isoc = &usb_urb->iso_frame_desc[i];
		st->in_packets++;
		if (usb_urb->status || isoc->status) {
			st->in_pkt_err++;
			dev_warn_ratelimited(dev, "in packet error %d\n",
					     isoc->status);
			n = mytek_pcm_fallback_frames(rt);
		} else if (isoc->actual_length < 4) {
			st->in_empty++;
			n = mytek_pcm_fallback_frames(rt);
		} else {
			n = (isoc->actual_length - 4) / (rt->in_n_analog << 2);
			st->in_frames_min = min(st->in_frames_min, n);
			st->in_frames_max = max(st->in_frames_max, n);
		}
		if (n > rt->out_max_frames) {
			st->in_clamped++;
			n = rt->out_max_frames;
		}
		frames[i] = n;
	}

	out_urb = mytek_pcm_get_idle_out_urb(rt);
	if (out_urb) {
		elapsed = mytek_pcm_fill_out_urb(rt, out_urb, frames);
		ret = usb_submit_urb(out_urb->instance, GFP_ATOMIC);
		if (ret) {
			set_bit(out_urb->index, &rt->out_idle);
			st->out_submit_err++;
			dev_warn_ratelimited(dev,
					     "out urb submit error %d\n", ret);
		}
	} else {
		st->out_starved++;
		dev_warn_ratelimited(dev, "no idle out urb\n");
	}
	if (elapsed) {
		st->periods++;
		snd_pcm_period_elapsed(elapsed);
	}

	if (rt->stream_state == STREAM_STARTING) {
		rt->stream_wait_cond = true;
		wake_up(&rt->stream_wait_queue);
	}

	ret = usb_submit_urb(usb_urb, GFP_ATOMIC);
	if (ret) {
		st->in_submit_err++;
		dev_warn_ratelimited(dev, "in urb submit error %d\n", ret);
	}
}

static void mytek_pcm_out_urb_handler(struct urb *usb_urb)
{
	struct pcm_urb *urb = usb_urb->context;
	struct pcm_runtime *rt = urb->chip->pcm;
	struct pcm_stats *st = &rt->stats;
	int bad = 0;
	int status = 0;
	int i;

	/* urbs cut short by usb_kill_urb() in stream_stop are not gaps */
	if (rt->stream_state == STREAM_STOPPING) {
		set_bit(urb->index, &rt->out_idle);
		return;
	}

	if (!usb_urb->status) {
		st->out_urbs++;
		for (i = 0; i < usb_urb->number_of_packets; i++)
			if (usb_urb->iso_frame_desc[i].status) {
				if (!bad)
					status = usb_urb->iso_frame_desc[i].status;
				bad++;
			}
		if (bad) {
			st->out_pkt_err += bad;
			dev_warn_ratelimited(&rt->chip->dev->dev,
				"%d out packets not sent (status %d), audible gap likely\n",
				bad, status);
		}
	} else if (!mytek_urb_killed(usb_urb->status)) {
		st->out_urb_err++;
		dev_warn_ratelimited(&rt->chip->dev->dev,
				     "out urb error %d\n", usb_urb->status);
	}

	set_bit(urb->index, &rt->out_idle);
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
		sub = &rt->playback;
	}

	if (!sub) {
		mutex_unlock(&rt->stream_mutex);
		dev_err(&rt->chip->dev->dev, "invalid stream type.\n");
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

static int mytek_pcm_prepare(struct snd_pcm_substream *alsa_sub)
{
	struct pcm_runtime *rt = snd_pcm_substream_chip(alsa_sub);
	struct pcm_substream *sub = mytek_pcm_get_substream(alsa_sub);
	struct snd_pcm_runtime *alsa_rt = alsa_sub->runtime;
	unsigned long flags;
	int ret;

	if (rt->panic)
		return -EPIPE;
	if (!sub)
		return -ENODEV;

	mutex_lock(&rt->stream_mutex);
	spin_lock_irqsave(&sub->lock, flags);
	sub->dma_off = 0;
	sub->period_off = 0;
	spin_unlock_irqrestore(&sub->lock, flags);

	/* hw_params may have changed the rate without closing the device */
	if (rt->stream_state != STREAM_DISABLED &&
			rates[rt->rate] != alsa_rt->rate)
		mytek_pcm_stream_stop(rt);

	if (rt->stream_state == STREAM_DISABLED) {
		for (rt->rate = 0; rt->rate < ARRAY_SIZE(rates); rt->rate++)
			if (alsa_rt->rate == rates[rt->rate])
				break;
		if (rt->rate == ARRAY_SIZE(rates)) {
			mutex_unlock(&rt->stream_mutex);
			dev_err(&rt->chip->dev->dev,
				"invalid rate %d in prepare.\n",
				alsa_rt->rate);
			return -EINVAL;
		}

		ret = mytek_pcm_set_rate(rt);
		if (ret) {
			rt->rate = ARRAY_SIZE(rates);
			mutex_unlock(&rt->stream_mutex);
			return ret;
		}
		ret = mytek_pcm_stream_start(rt);
		if (ret) {
			rt->rate = ARRAY_SIZE(rates);
			mutex_unlock(&rt->stream_mutex);
			dev_err(&rt->chip->dev->dev,
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
		return SNDRV_PCM_POS_XRUN;

	spin_lock_irqsave(&sub->lock, flags);
	ret = sub->dma_off;
	spin_unlock_irqrestore(&sub->lock, flags);

	return ret;
}

static const struct snd_pcm_ops pcm_ops = {
	.open = mytek_pcm_open,
	.close = mytek_pcm_close,
	.prepare = mytek_pcm_prepare,
	.trigger = mytek_pcm_trigger,
	.pointer = mytek_pcm_pointer,
};

static void mytek_pcm_proc_read(struct snd_info_entry *entry,
		struct snd_info_buffer *buffer)
{
	struct pcm_runtime *rt = entry->private_data;
	struct pcm_stats *st = &rt->stats;
	u8 state = READ_ONCE(rt->stream_state);
	u8 rate = READ_ONCE(rt->rate);

	snd_iprintf(buffer, "driver_version: %s\n", MYTEK_DRIVER_VERSION);
	snd_iprintf(buffer, "state: %s\n", state < ARRAY_SIZE(stream_state_names)
		    ? stream_state_names[state] : "?");
	snd_iprintf(buffer, "rate: %d\n", rate < ARRAY_SIZE(rates)
		    ? rates[rate] : 0);
	snd_iprintf(buffer, "out_lead: %u\n", rt->out_lead);
	snd_iprintf(buffer, "zlp_fill: %u\n", rt->zlp_fill);
	snd_iprintf(buffer, "in_packet_size: %d\n", rt->in_packet_size);
	snd_iprintf(buffer, "out_packet_size: %d\n", rt->out_packet_size);
	snd_iprintf(buffer, "packets_per_sec: %u\n", rt->packets_per_sec);
	snd_iprintf(buffer, "seconds: %lld\n",
		    ktime_ms_delta(ktime_get(), st->started) / 1000);
	snd_iprintf(buffer, "in_urbs: %llu\n", st->in_urbs);
	snd_iprintf(buffer, "in_packets: %llu\n", st->in_packets);
	snd_iprintf(buffer, "in_empty: %llu\n", st->in_empty);
	snd_iprintf(buffer, "in_pkt_err: %llu\n", st->in_pkt_err);
	snd_iprintf(buffer, "in_urb_err: %llu\n", st->in_urb_err);
	snd_iprintf(buffer, "in_submit_err: %llu\n", st->in_submit_err);
	snd_iprintf(buffer, "in_clamped: %llu\n", st->in_clamped);
	snd_iprintf(buffer, "in_late: %llu\n", st->in_late);
	snd_iprintf(buffer, "in_gap_max_us: %u\n", st->in_gap_max_us);
	snd_iprintf(buffer, "in_frames_min: %u\n",
		    st->in_frames_min == U32_MAX ? 0 : st->in_frames_min);
	snd_iprintf(buffer, "in_frames_max: %u\n", st->in_frames_max);
	snd_iprintf(buffer, "out_urbs: %llu\n", st->out_urbs);
	snd_iprintf(buffer, "out_frames: %llu\n", st->out_frames);
	snd_iprintf(buffer, "out_pkt_err: %llu\n", st->out_pkt_err);
	snd_iprintf(buffer, "out_urb_err: %llu\n", st->out_urb_err);
	snd_iprintf(buffer, "out_submit_err: %llu\n", st->out_submit_err);
	snd_iprintf(buffer, "out_starved: %llu\n", st->out_starved);
	snd_iprintf(buffer, "periods: %llu\n", st->periods);
}

static void mytek_pcm_free(struct pcm_runtime *rt)
{
	int i;

	for (i = 0; i < PCM_N_URBS; i++) {
		usb_free_urb(rt->out_urbs[i].instance);
		usb_free_urb(rt->in_urbs[i].instance);
		kfree(rt->out_urbs[i].buffer);
		kfree(rt->in_urbs[i].buffer);
	}
	kfree(rt);
}

static int mytek_pcm_init_urb(struct pcm_urb *urb,
		struct mytek_chip *chip, bool in, int ep, int index,
		void (*handler)(struct urb *))
{
	urb->chip = chip;
	urb->index = index;
	urb->buffer = kzalloc(PCM_N_PACKETS_PER_URB * PCM_MAX_PACKET_SIZE,
			GFP_KERNEL);
	if (!urb->buffer)
		return -ENOMEM;
	urb->instance = usb_alloc_urb(PCM_N_PACKETS_PER_URB, GFP_KERNEL);
	if (!urb->instance)
		return -ENOMEM;
	urb->instance->transfer_buffer = urb->buffer;
	urb->instance->transfer_buffer_length =
			PCM_N_PACKETS_PER_URB * PCM_MAX_PACKET_SIZE;
	urb->instance->dev = chip->dev;
	urb->instance->pipe = in ? usb_rcvisocpipe(chip->dev, ep)
			: usb_sndisocpipe(chip->dev, ep);
	urb->instance->interval = 1;
	urb->instance->complete = handler;
	urb->instance->context = urb;
	urb->instance->number_of_packets = PCM_N_PACKETS_PER_URB;
	return 0;
}

int mytek_pcm_init(struct mytek_chip *chip)
{
	int i;
	int ret;
	struct snd_pcm *pcm;
	struct snd_info_entry *entry;
	struct pcm_runtime *rt =
			kzalloc(sizeof(struct pcm_runtime), GFP_KERNEL);

	if (!rt)
		return -ENOMEM;

	rt->chip = chip;
	rt->stream_state = STREAM_DISABLED;
	rt->rate = ARRAY_SIZE(rates);
	rt->urb_interval = 1;
	rt->packets_per_sec = 8000;
	rt->out_lead = out_lead;
	rt->zlp_fill = zlp_fill;
	init_waitqueue_head(&rt->stream_wait_queue);
	mutex_init(&rt->stream_mutex);
	mytek_pcm_reset_stats(rt);

	spin_lock_init(&rt->playback.lock);

	for (i = 0; i < PCM_N_URBS; i++) {
		ret = mytek_pcm_init_urb(&rt->in_urbs[i], chip, true, IN_EP, i,
				mytek_pcm_in_urb_handler);
		if (ret)
			goto error;
		ret = mytek_pcm_init_urb(&rt->out_urbs[i], chip, false, OUT_EP,
				i, mytek_pcm_out_urb_handler);
		if (ret)
			goto error;
	}

	ret = snd_pcm_new(chip->card, "MytekUSB2", 0, 1, 0, &pcm);
	if (ret < 0) {
		dev_err(&chip->dev->dev, "cannot create pcm instance.\n");
		goto error;
	}

	pcm->private_data = rt;
	strscpy(pcm->name, "Mytek USB2", sizeof(pcm->name));
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &pcm_ops);
	snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_VMALLOC, NULL, 0, 0);

	if (!snd_card_proc_new(chip->card, "mytek", &entry))
		snd_info_set_text_ops(entry, rt, mytek_pcm_proc_read);

	rt->instance = pcm;
	chip->pcm = rt;
	return 0;

error:
	mytek_pcm_free(rt);
	return ret;
}

void mytek_pcm_abort(struct mytek_chip *chip)
{
	struct pcm_runtime *rt = chip->pcm;
	int i;

	if (rt) {
		rt->panic = true;

		if (rt->playback.instance)
			snd_pcm_stop_xrun(rt->playback.instance);

		for (i = 0; i < PCM_N_URBS; i++) {
			usb_poison_urb(rt->in_urbs[i].instance);
			usb_poison_urb(rt->out_urbs[i].instance);
		}
	}
}

void mytek_pcm_destroy(struct mytek_chip *chip)
{
	struct pcm_runtime *rt = chip->pcm;

	if (!rt)
		return;
	mytek_pcm_free(rt);
	chip->pcm = NULL;
}
