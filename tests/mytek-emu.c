// SPDX-License-Identifier: GPL-2.0
/*
 * Fake Mytek Stereo192-DSD USB2 device for testing snd-usb-mytek without
 * hardware. Attaches itself to the vhci_hcd (USB/IP client) driver through
 * a socketpair and speaks the USB/IP protocol on the other end.
 *
 * The device pretends to have all firmware loaded. It generates feedback
 * (iso IN) packets at the nominal rate of the selected sample rate and
 * consumes iso OUT urbs at the same pace, verifying the playback data:
 * the test signal is a ramp, left = n, right = ~n (24 bit), n = 1, 2, ...
 * DoP streams (detected by the first frame after SET_INTERFACE: DSD
 * silence 0x69 with a DoP marker) are checked as DoP: markers 0x05/0xfa
 * alternating every frame in both channels, the 16 DSD bits per channel
 * (earlier byte in the middle byte) a ramp left = n, right = ~n (16 bit);
 * 0x6969 = silence.
 * The comm sequence of each stream is compared with the Windows driver's
 * (USB capture): rate setup, streaming on, unmute (GPIO 0 high, reg 6 =
 * 2), and at the end mute (GPIO 0 low, reg 6 = 0), streaming off.
 *
 * Usage: mytek-emu [-z every_n_packets_zlp] [-e every_n_packets_err]
 *                  [-x every_n_out_urbs_exdev] [-d seconds_until_disconnect]
 * SIGUSR1 prints and resets the playback statistics.
 *
 * Build: gcc -O2 -static -o mytek-emu mytek-emu.c
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#define CMD_SUBMIT 1
#define CMD_UNLINK 2
#define RET_SUBMIT 3
#define RET_UNLINK 4
#define DIR_OUT 0
#define DIR_IN 1
#define MAX_PENDING 256
#define MAX_PKTS 64

struct hdr {
	uint32_t command, seqnum, devid, direction, ep;
	union {
		struct {
			uint32_t flags;
			int32_t len, start_frame, npackets, interval;
			uint8_t setup[8];
		} __attribute__((packed)) submit;
		struct {
			int32_t status, actual_length, start_frame, npackets,
				error_count;
		} __attribute__((packed)) ret;
		struct { uint32_t seqnum; } __attribute__((packed)) unlink;
		uint8_t pad[28];
	} u;
} __attribute__((packed));

struct isodesc { uint32_t offset, length, actual_length, status; };

struct pending {
	uint32_t seqnum;
	int ep, dir;
	int len, npackets;
	struct isodesc iso[MAX_PKTS];
	uint8_t *data;
};

static int fd;
static struct pending pend[MAX_PENDING];
static int npend;
static volatile sig_atomic_t dump_req;

/* device state */
static int altsetting;
static int stream_alt; /* last alt setting != 0 */
/* alt 4: 352.8/384k; 5/6 (vendor native DSD, unused): 44.1k/88.2k frames */
static const int alt_rate_base[] = { 0, 44100, 88200, 176400, 352800,
				     44100, 88200 };
static int rate_reg_vh = -1, rate_reg_vl = -1;
/* FPGA regs 2 (EN_I2S: out mask << 8 | in mask) and 4 (EN_DSD) */
static int en_i2s = 0x0707, en_dsd = 0x0006, reg6;
/* comm writes since the last SET_INTERFACE: "Wrr=vvvv" FPGA register
 * writes, "Grr=vv" GPIO set; register reads (reg | 0x80) are not logged */
static char seq[64][12];
static long seq_ms[64];
static int seq_n;
static int streaming;
static struct timespec seq_t0;
/* stream type from the first frame after SET_INTERFACE: -1 unknown */
static int dop_stream = -1;
static int last_marker = -1;

/* injection */
static long zlp_every, err_every, exdev_every;
static long in_pkt_count, out_urb_count;

/* verification */
static unsigned long long frames_rx, silence_rx, disc, bad_hdr, bad_marker,
	in_urbs_tx, out_urbs_rx, underruns, zlp_injected, err_injected,
	exdev_injected, pkt_frames_hist[64], dop_frames_rx, dop_exit_rx;
static uint32_t last_l;
static int have_last;
static int out_started;
static unsigned long nominal_acc;

static const uint8_t dev_desc[18] = {
	18, 1, 0x00, 0x02, 0xff, 0x00, 0x00, 64,
	0xce, 0x25, 0x0e, 0x00, 0x00, 0x01, 0, 0, 0, 1
};

static uint8_t cfg_desc[256];
static int cfg_len;

static void add(const uint8_t *d, int n)
{
	memcpy(cfg_desc + cfg_len, d, n);
	cfg_len += n;
}

static void ep(uint8_t addr, uint8_t attr, uint16_t maxp, uint8_t interval)
{
	uint8_t d[7] = { 7, 5, addr, attr, maxp & 0xff, maxp >> 8, interval };

	add(d, 7);
}

static void iface(uint8_t num, uint8_t alt, uint8_t neps)
{
	uint8_t d[9] = { 9, 4, num, alt, neps, 0xff, 0, 0, 0 };

	add(d, 9);
}

static void build_cfg(void)
{
	uint8_t c[9] = { 9, 2, 0, 0, 2, 1, 0, 0x80, 50 };

	add(c, 9);
	iface(0, 0, 3);
	ep(0x81, 3, 64, 1);
	ep(0x01, 3, 64, 1);
	ep(0x02, 2, 512, 0);
	iface(1, 0, 0);
	iface(1, 1, 2);
	ep(0x82, 0x05, 228, 1);
	ep(0x06, 0x05, 228, 1);
	iface(1, 2, 2);
	ep(0x82, 0x05, 420, 1);
	ep(0x06, 0x05, 420, 1);
	iface(1, 3, 2);
	ep(0x82, 0x05, 404, 1);
	ep(0x06, 0x05, 604, 1);
	/* alts 4-6 as on the real device: firmware default 1024 */
	iface(1, 4, 2);
	ep(0x82, 0x05, 1024, 1);
	ep(0x06, 0x05, 1024, 1);
	iface(1, 5, 2);
	ep(0x82, 0x05, 1024, 1);
	ep(0x06, 0x05, 1024, 1);
	iface(1, 6, 2);
	ep(0x82, 0x05, 1024, 1);
	ep(0x06, 0x05, 1024, 1);
	cfg_desc[2] = cfg_len & 0xff;
	cfg_desc[3] = cfg_len >> 8;
}

static int xread(void *buf, size_t n)
{
	size_t got = 0;

	while (got < n) {
		ssize_t r = read(fd, (char *)buf + got, n - got);

		if (r <= 0)
			return -1;
		got += r;
	}
	return 0;
}

static void xwrite(const void *buf, size_t n)
{
	size_t done = 0;

	while (done < n) {
		ssize_t r = write(fd, (const char *)buf + done, n - done);

		if (r <= 0) {
			perror("write");
			exit(1);
		}
		done += r;
	}
}

static void send_ret(uint32_t seqnum, int dir, int ep, int status,
		     const uint8_t *data, int len, int npackets,
		     struct isodesc *iso)
{
	struct hdr h;
	int i;

	memset(&h, 0, sizeof(h));
	h.command = htonl(RET_SUBMIT);
	h.seqnum = htonl(seqnum);
	h.direction = htonl(dir);
	h.ep = htonl(ep);
	h.u.ret.status = htonl(status);
	h.u.ret.actual_length = htonl(len);
	h.u.ret.npackets = htonl(npackets);
	xwrite(&h, sizeof(h));
	if (dir == DIR_IN && len > 0)
		xwrite(data, len);
	for (i = 0; i < npackets; i++) {
		struct isodesc d = {
			htonl(iso[i].offset), htonl(iso[i].length),
			htonl(iso[i].actual_length), htonl(iso[i].status)
		};
		xwrite(&d, sizeof(d));
	}
}

static int popcount8(int v)
{
	return __builtin_popcount(v & 0xff);
}

static int out_slots(void)
{
	return 2 * popcount8(en_i2s >> 8);
}

static int in_slots(void)
{
	return 2 * popcount8(en_i2s);
}

static int cur_rate(void)
{
	/* reg 1: vh 0x11 -> 44.1/48, 0x10 -> 88.2/96, 0x00 -> 176.4/192 */
	int base = alt_rate_base[altsetting ? altsetting : stream_alt];

	if (rate_reg_vl == 1)
		base = base / 147 * 160;
	return base;
}

static void handle_control(struct hdr *h, uint8_t *out_data)
{
	uint8_t *s = h->u.submit.setup;
	int reqtype = s[0], req = s[1];
	int value = s[2] | s[3] << 8, index = s[4] | s[5] << 8;
	int wlen = s[6] | s[7] << 8;
	uint8_t buf[512];
	int len = 0, status = 0;

	(void)out_data;
	if ((reqtype & 0x60) == 0) { /* standard */
		if (req == 6) { /* GET_DESCRIPTOR */
			if (value >> 8 == 1) {
				memcpy(buf, dev_desc, 18);
				len = 18;
			} else if (value >> 8 == 2) {
				memcpy(buf, cfg_desc, cfg_len);
				len = cfg_len;
			} else {
				status = -32;
			}
		} else if (req == 11) { /* SET_INTERFACE */
			if (index == 1) {
				altsetting = value;
				/* alt 0 = idle between streams (2.2.1+) */
				if (value) {
					stream_alt = value;
					seq_n = 0;
					dop_stream = -1;
					last_marker = -1;
					clock_gettime(CLOCK_MONOTONIC, &seq_t0);
				}
				fprintf(stderr, "emu: interface 1 alt %d\n",
					value);
			}
		} else if (req == 0) { /* GET_STATUS */
			buf[0] = buf[1] = 0;
			len = 2;
		} else if (req != 9) { /* SET_CONFIGURATION is fine */
			status = -32;
		}
	} else if ((reqtype & 0x60) == 0x40) { /* vendor */
		if (reqtype & 0x80) {
			if (req == 1) {
				const uint8_t st[8] = { 0xeb, 0xaa, 0x55, 0x03,
					0x03, 0x01, 0x23, 0x16 };
				memcpy(buf, st, 8);
				len = 8;
			} else if (req == 2) {
				buf[0] = 0;
				len = 1;
			} else {
				status = -32;
			}
		}
	}
	if (len > wlen)
		len = wlen;
	if (!(reqtype & 0x80))
		len = h->u.submit.len;
	send_ret(h->seqnum, h->direction, 0, status, buf, len, 0, NULL);
}

static void seq_add(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	seq_ms[seq_n] = (now.tv_sec - seq_t0.tv_sec) * 1000 +
			(now.tv_nsec - seq_t0.tv_nsec) / 1000000;
	seq_n++;
}

static void handle_comm(const uint8_t *b, int len)
{
	/* 01 len req id reg vl vh */
	if (len >= 6 && b[0] == 0x01 && b[2] == 0x22 && seq_n < 64) {
		snprintf(seq[seq_n], sizeof(seq[0]), "G%02x=%02x", b[4], b[5]);
		seq_add();
	}
	if (len >= 7 && b[0] == 0x01 && b[2] == 0x02 && !(b[4] & 0x80) &&
	    seq_n < 64) {
		snprintf(seq[seq_n], sizeof(seq[0]), "W%02x=%02x%02x", b[4],
			 b[5], b[6]);
		seq_add();
	}
	if (len >= 7 && b[0] == 0x01 && b[2] == 0x02) {
		if (b[4] == 0x01) {
			rate_reg_vl = b[5];
			rate_reg_vh = b[6];
		} else if (b[4] == 0x00) {
			streaming = b[6];
		} else if (b[4] == 0x02) {
			en_i2s = b[5] << 8 | b[6];
		} else if (b[4] == 0x04) {
			en_dsd = b[5] << 8 | b[6];
		} else if (b[4] == 0x06) {
			reg6 = b[5] << 8 | b[6];
		}
	}
}

static void verify_dop_frame(const uint8_t *fr)
{
	int m = fr[2];
	uint32_t l = fr[1] << 8 | fr[0], r = fr[5] << 8 | fr[4];

	frames_rx++;
	dop_frames_rx++;
	/* dop_exit: PCM silence (no marker) before the stream stops */
	if (!(fr[0] | fr[1] | fr[2] | fr[4] | fr[5] | fr[6])) {
		silence_rx++;
		dop_exit_rx++;
		last_marker = -1;
		return;
	}
	if ((m != 0x05 && m != 0xfa) || fr[6] != m || m == last_marker)
		bad_marker++;
	last_marker = m;
	if (l == 0x6969 && r == 0x6969) {
		silence_rx++;
		return;
	}
	if (r != (~l & 0xffff) ||
	    (have_last && l != ((last_l + 1) & 0xffff)) ||
	    (!have_last && l != 1))
		disc++;
	last_l = l;
	have_last = 1;
}

static void verify_out(struct pending *p)
{
	int i;
	int slots = out_slots();

	for (i = 0; i < p->npackets; i++) {
		const uint8_t *b = p->data + p->iso[i].offset;
		int plen = p->iso[i].length;
		int n, f, ch;

		if (plen < 4 || b[0] != 0xaa || b[1] != 0xaa || b[3] != 0) {
			bad_hdr++;
			continue;
		}
		n = b[2];
		if (plen != 4 + n * slots * 4) {
			bad_hdr++;
			continue;
		}
		if (n < 64)
			pkt_frames_hist[n]++;
		for (f = 0; f < n; f++) {
			const uint8_t *fr = b + 4 + f * slots * 4;
			uint32_t l = fr[0] | fr[1] << 8 | fr[2] << 16;
			uint32_t r = fr[4] | fr[5] << 8 | fr[6] << 16;

			for (ch = 0; ch < slots; ch++)
				if (fr[ch * 4 + 3] != 0x40)
					bad_marker++;
			if (dop_stream < 0)
				dop_stream = fr[0] == 0x69 &&
					     (fr[2] == 0x05 || fr[2] == 0xfa);
			if (dop_stream) {
				verify_dop_frame(fr);
				continue;
			}
			frames_rx++;
			if (l == 0 && r == 0) {
				silence_rx++;
				continue;
			}
			if (r != (~l & 0xffffff) ||
			    (have_last && l != ((last_l + 1) & 0xffffff)) ||
			    (!have_last && l != 1))
				disc++;
			last_l = l;
			have_last = 1;
		}
	}
}

/*
 * Register sequence of one stream as the Windows driver sends it (USB
 * capture 2026-10-18, minus its read-backs): GPIO 0 low, streaming off,
 * CLK_SEL, streaming off, OUT_DEL, EN_I2S, EN_DSD, EN_SPDIF, reg 9,
 * streaming on, unmute (GPIO 0 high, reg 6 = 2); at the end mute (GPIO 0
 * low, reg 6 = 0), streaming off. Returns the ms from streaming on to the
 * unmute, or -1 on a mismatch.
 */
static long check_seq(void)
{
	char exp[15][12] = { "G00=00", "W00=0000", "", "W00=0000",
			     "W05=0000", "", "", "W03=0000", "W09=0001",
			     "W00=0001", "G00=01", "W06=0002", "G00=00",
			     "W06=0000", "W00=0000" };
	int i;

	snprintf(exp[2], sizeof(exp[2]), "W01=%02x%02x", rate_reg_vl & 0xff,
		 rate_reg_vh & 0xff);
	snprintf(exp[5], sizeof(exp[5]), "W02=%04x", en_i2s);
	snprintf(exp[6], sizeof(exp[6]), "W04=%04x", en_dsd);
	if (seq_n != 15)
		return -1;
	for (i = 0; i < 15; i++)
		if (strcmp(seq[i], exp[i]))
			return -1;
	return seq_ms[10] - seq_ms[9];
}

static void dump_stats(const char *why)
{
	int i;

	long unmute_ms = check_seq();

	printf("EMU-SEQ:");
	for (i = 0; i < seq_n; i++)
		printf(" %s", seq[i]);
	if (unmute_ms >= 0)
		printf(" | vendor_seq=OK unmute_after=%ldms\n", unmute_ms);
	else
		printf(" | vendor_seq=MISMATCH\n");

	printf("EMU-MODE: alt=%d clk=0x%02x%02x en_i2s=0x%04x en_dsd=0x%04x reg6=0x%04x dop=%d dop_frames=%llu dop_exit_frames=%llu\n",
	       stream_alt, rate_reg_vl & 0xff, rate_reg_vh & 0xff, en_i2s,
	       en_dsd, reg6, dop_stream, dop_frames_rx, dop_exit_rx);
	printf("EMU-STATS(%s): rate=%d in_urbs=%llu out_urbs=%llu frames=%llu silence=%llu signal=%llu discontinuities=%llu bad_hdr=%llu bad_marker=%llu underruns=%llu zlp_inj=%llu err_inj=%llu exdev_inj=%llu last_sample=%u\n",
	       why, cur_rate(), in_urbs_tx, out_urbs_rx, frames_rx, silence_rx,
	       frames_rx - silence_rx, disc, bad_hdr, bad_marker, underruns,
	       zlp_injected, err_injected, exdev_injected, last_l);
	printf("EMU-HIST:");
	for (i = 0; i < 64; i++)
		if (pkt_frames_hist[i])
			printf(" %d:%llu", i, pkt_frames_hist[i]);
	printf("\n");
	fflush(stdout);
	frames_rx = silence_rx = disc = bad_hdr = bad_marker = 0;
	in_urbs_tx = out_urbs_rx = underruns = dop_frames_rx = dop_exit_rx = 0;
	zlp_injected = err_injected = exdev_injected = 0;
	memset(pkt_frames_hist, 0, sizeof(pkt_frames_hist));
	have_last = 0;
	last_l = 0;
	out_started = 0;
}

static void remove_pending(int i)
{
	free(pend[i].data);
	pend[i] = pend[--npend];
}

/* oldest pending urb for an endpoint (seqnums increase) */
static int find_oldest(int epn, int dir)
{
	int i, best = -1;

	for (i = 0; i < npend; i++)
		if (pend[i].ep == epn && pend[i].dir == dir &&
		    (best < 0 || pend[i].seqnum < pend[best].seqnum))
			best = i;
	return best;
}

static void tick(void)
{
	int i, k;

	/* feedback: one in urb (8 packets) per ms */
	i = find_oldest(2, DIR_IN);
	if (i >= 0) {
		struct pending *p = &pend[i];
		uint8_t buf[MAX_PKTS * 1024];
		int total = 0;
		int rate = cur_rate();

		for (k = 0; k < p->npackets; k++) {
			int frames;
			int len;

			nominal_acc += rate;
			frames = nominal_acc / 8000;
			nominal_acc %= 8000;
			len = 4 + frames * in_slots() * 4;
			in_pkt_count++;
			p->iso[k].status = 0;
			if (zlp_every && in_pkt_count % zlp_every == 0) {
				len = 0;
				zlp_injected++;
			} else if (err_every && in_pkt_count % err_every == 0) {
				len = 0;
				p->iso[k].status = -18; /* EXDEV */
				err_injected++;
			}
			if (len > (int)p->iso[k].length)
				len = p->iso[k].length;
			if (len < 0)
				len = 0;
			memset(buf + total, 0, len);
			if (len >= 4) {
				buf[total] = 0xaa;
				buf[total + 1] = 0xaa;
				buf[total + 2] = frames;
			}
			p->iso[k].actual_length = len;
			total += len;
		}
		send_ret(p->seqnum, DIR_IN, 2, 0, buf, total, p->npackets,
			 p->iso);
		in_urbs_tx++;
		remove_pending(i);
	}

	/* playback: one out urb per ms */
	i = find_oldest(6, DIR_OUT);
	if (i >= 0) {
		struct pending *p = &pend[i];
		int total = 0;
		int ex = 0;

		out_started = 1;
		out_urb_count++;
		if (exdev_every && out_urb_count % exdev_every == 0) {
			ex = 1;
			exdev_injected++;
		}
		verify_out(p);
		for (k = 0; k < p->npackets; k++) {
			p->iso[k].actual_length = ex ? 0 : p->iso[k].length;
			p->iso[k].status = ex ? -18 : 0;
			total += p->iso[k].actual_length;
		}
		send_ret(p->seqnum, DIR_OUT, 6, 0, NULL, total, p->npackets,
			 p->iso);
		out_urbs_rx++;
		remove_pending(i);
	} else if (out_started && streaming) {
		underruns++;
	}
}

static void handle_msg(void)
{
	struct hdr h;
	struct pending *p;
	int i;

	if (xread(&h, sizeof(h)) < 0) {
		dump_stats("eof");
		exit(0);
	}
	h.command = ntohl(h.command);
	h.seqnum = ntohl(h.seqnum);
	h.direction = ntohl(h.direction);
	h.ep = ntohl(h.ep);

	if (h.command == CMD_UNLINK) {
		struct hdr r;
		uint32_t target = ntohl(h.u.unlink.seqnum);
		int status = 0;

		for (i = 0; i < npend; i++)
			if (pend[i].seqnum == target) {
				remove_pending(i);
				status = -104; /* ECONNRESET */
				break;
			}
		memset(&r, 0, sizeof(r));
		r.command = htonl(RET_UNLINK);
		r.seqnum = htonl(h.seqnum);
		r.u.ret.status = htonl(status);
		xwrite(&r, sizeof(r));
		return;
	}
	if (h.command != CMD_SUBMIT) {
		fprintf(stderr, "emu: unknown command %u\n", h.command);
		exit(1);
	}
	h.u.submit.len = ntohl(h.u.submit.len);
	h.u.submit.npackets = ntohl(h.u.submit.npackets);
	if (h.u.submit.npackets < 0 || h.u.submit.npackets == (int32_t)0xffffffff)
		h.u.submit.npackets = 0;

	if (npend >= MAX_PENDING) {
		fprintf(stderr, "emu: too many pending urbs\n");
		exit(1);
	}
	p = &pend[npend];
	memset(p, 0, sizeof(*p));
	p->seqnum = h.seqnum;
	p->ep = h.ep;
	p->dir = h.direction;
	p->len = h.u.submit.len;
	p->npackets = h.u.submit.npackets;
	if (p->npackets > MAX_PKTS) {
		fprintf(stderr, "emu: too many packets\n");
		exit(1);
	}
	p->data = calloc(1, p->len > 0 ? p->len : 1);
	if (p->dir == DIR_OUT && p->len > 0 && xread(p->data, p->len) < 0)
		exit(0);
	for (i = 0; i < p->npackets; i++) {
		struct isodesc d;

		if (xread(&d, sizeof(d)) < 0)
			exit(0);
		p->iso[i].offset = ntohl(d.offset);
		p->iso[i].length = ntohl(d.length);
	}

	if (p->ep == 0) {
		handle_control(&h, p->data);
		free(p->data);
		return;
	}
	if (p->ep == 1 && p->dir == DIR_OUT) { /* comm write */
		handle_comm(p->data, p->len);
		send_ret(p->seqnum, DIR_OUT, 1, 0, NULL, p->len, 0, NULL);
		free(p->data);
		return;
	}
	if (p->ep == 2 && p->dir == DIR_OUT) { /* fpga bulk */
		send_ret(p->seqnum, DIR_OUT, 2, 0, NULL, p->len, 0, NULL);
		free(p->data);
		return;
	}
	/* ep1 in (comm receiver), ep2 in and ep6 out (iso) stay pending */
	npend++;
}

static void on_usr1(int sig)
{
	(void)sig;
	dump_req = 1;
}

int main(int argc, char **argv)
{
	int sv[2], tfd, opt, afd;
	long disconnect_after = 0;
	struct itimerspec its = { { 0, 1000000 }, { 0, 1000000 } };
	struct timespec t0, now;
	char buf[64];

	while ((opt = getopt(argc, argv, "z:e:x:d:")) != -1) {
		switch (opt) {
		case 'z': zlp_every = atol(optarg); break;
		case 'e': err_every = atol(optarg); break;
		case 'x': exdev_every = atol(optarg); break;
		case 'd': disconnect_after = atol(optarg); break;
		default: return 2;
		}
	}
	build_cfg();
	signal(SIGUSR1, on_usr1);
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
		perror("socketpair");
		return 1;
	}
	fd = sv[0];
	afd = open("/sys/devices/platform/vhci_hcd.0/attach", O_WRONLY);
	if (afd < 0) {
		perror("open attach");
		return 1;
	}
	/* port 0 (high speed hub), sockfd, devid, speed 3 = high */
	snprintf(buf, sizeof(buf), "0 %d %d 3", sv[1], (1 << 16) | 2);
	if (write(afd, buf, strlen(buf)) < 0) {
		perror("attach");
		return 1;
	}
	close(afd);
	close(sv[1]);
	fprintf(stderr, "emu: attached\n");

	tfd = timerfd_create(CLOCK_MONOTONIC, 0);
	timerfd_settime(tfd, 0, &its, NULL);
	clock_gettime(CLOCK_MONOTONIC, &t0);

	for (;;) {
		struct pollfd pfd[2] = { { fd, POLLIN, 0 }, { tfd, POLLIN, 0 } };

		if (poll(pfd, 2, -1) < 0 && errno != EINTR)
			return 1;
		if (dump_req) {
			dump_req = 0;
			dump_stats("usr1");
		}
		if (pfd[0].revents & (POLLIN | POLLHUP))
			handle_msg();
		if (pfd[1].revents & POLLIN) {
			uint64_t exp;

			if (read(tfd, &exp, sizeof(exp)) == sizeof(exp))
				while (exp--)
					tick();
		}
		if (disconnect_after) {
			clock_gettime(CLOCK_MONOTONIC, &now);
			if (now.tv_sec - t0.tv_sec >= disconnect_after) {
				dump_stats("disconnect");
				fprintf(stderr, "emu: disconnecting\n");
				return 0;
			}
		}
	}
}
