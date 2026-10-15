/*
 * Linux driver for Mytek Digital Stereo192-DSD DAC USB2
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

#ifndef MYTEK_PCM_H
#define MYTEK_PCM_H

#include <sound/pcm.h>
#include <linux/mutex.h>
#include <linux/ktime.h>

#include "common.h"

enum /* settings for pcm */
{
	/* maximum of EP_W_MAX_PACKET_SIZE[] (see firmware.c) */
	PCM_N_URBS = 16, PCM_N_PACKETS_PER_URB = 8, PCM_MAX_PACKET_SIZE = 604
};

struct pcm_urb {
	struct mytek_chip *chip;

	struct urb *instance;
	u8 *buffer;
	int index;
};

struct pcm_substream {
	spinlock_t lock;
	struct snd_pcm_substream *instance;

	bool active;

	snd_pcm_uframes_t dma_off; /* current position in alsa dma_area */
	snd_pcm_uframes_t period_off; /* current position in current period */
};

/*
 * Streaming statistics, reset on every stream start. Exposed in
 * /proc/asound/cardX/mytek and summarised in the kernel log on stream stop.
 * Updated from urb completion handlers without locking: values are
 * diagnostic only.
 */
struct pcm_stats {
	u64 in_urbs;		/* completed in (feedback) urbs */
	u64 in_packets;		/* in packets processed */
	u64 in_empty;		/* in packets with no frames (zero length) */
	u64 in_pkt_err;		/* in packets with error status */
	u64 in_urb_err;		/* in urbs completed with error status */
	u64 in_submit_err;	/* in urb resubmission failures */
	u64 in_clamped;		/* in packets asking for > out maxpacket */
	u64 in_late;		/* in urb completions > 2ms after previous */
	u32 in_gap_max_us;	/* largest gap between in urb completions */
	u32 in_frames_min;	/* smallest frame count in a valid in packet */
	u32 in_frames_max;	/* largest frame count in a valid in packet */
	u64 out_urbs;		/* completed out (playback) urbs */
	u64 out_frames;		/* frames sent to the device (incl. silence) */
	u64 out_pkt_err;	/* out packets not transmitted (e.g. missed) */
	u64 out_urb_err;	/* out urbs completed with error status */
	u64 out_submit_err;	/* out urb submission failures */
	u64 out_starved;	/* no idle out urb available */
	u64 periods;		/* alsa periods elapsed */
	ktime_t last_in;
	ktime_t started;
};

struct pcm_runtime {
	struct mytek_chip *chip;
	struct snd_pcm *instance;

	struct pcm_substream playback;
	bool panic; /* if set driver won't do anymore pcm on device */

	struct pcm_urb in_urbs[PCM_N_URBS];
	struct pcm_urb out_urbs[PCM_N_URBS];
	unsigned long out_idle; /* bitmap of out urbs not owned by the hcd */
	int out_next; /* next out urb to try */

	int in_packet_size;
	int out_packet_size;
	int in_n_analog; /* number of analog channels soundcard sends */
	int out_n_analog; /* number of analog channels soundcard receives */
	unsigned int out_max_frames; /* frames that fit into an out packet */
	unsigned int urb_interval; /* urb->interval to use for iso urbs */
	unsigned int packets_per_sec; /* iso packets per second */
	unsigned int nominal_acc; /* accumulator for nominal frame counts */
	unsigned int zlp_fill; /* zlp_fill module param used for stream */
	unsigned int out_lead; /* out_lead module param used for stream */

	struct mutex stream_mutex;
	u8 stream_state; /* one of STREAM_XXX (pcm.c) */
	u8 rate; /* one of PCM_RATE_XXX */
	wait_queue_head_t stream_wait_queue;
	bool stream_wait_cond;

	struct pcm_stats stats;
};

int mytek_pcm_init(struct mytek_chip *chip);
void mytek_pcm_abort(struct mytek_chip *chip);
void mytek_pcm_destroy(struct mytek_chip *chip);
#endif /* MYTEK_PCM_H */
