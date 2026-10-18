// SPDX-License-Identifier: GPL-2.0
/*
 * Print which sample rates an ALSA playback device accepts per format,
 * without playing anything (aplay -r silently falls back to the nearest
 * rate, so it cannot show a refusal).
 *
 * Usage: alsa-rates DEVICE FORMAT...   e.g. alsa-rates hw:0,0 S32_LE DSD_U8
 * Build: gcc -O2 -o alsa-rates alsa-rates.c -l:libasound.so.2
 * (prototypes declared here, so no libasound development headers needed)
 */
#include <stdio.h>
#include <string.h>

typedef struct _snd_pcm snd_pcm_t;
typedef struct _snd_pcm_hw_params snd_pcm_hw_params_t;

int snd_pcm_open(snd_pcm_t **pcm, const char *name, int stream, int mode);
int snd_pcm_close(snd_pcm_t *pcm);
size_t snd_pcm_hw_params_sizeof(void);
int snd_pcm_hw_params_any(snd_pcm_t *pcm, snd_pcm_hw_params_t *params);
int snd_pcm_hw_params_set_channels(snd_pcm_t *pcm,
		snd_pcm_hw_params_t *params, unsigned int val);
int snd_pcm_hw_params_set_format(snd_pcm_t *pcm,
		snd_pcm_hw_params_t *params, int format);
int snd_pcm_hw_params_test_rate(snd_pcm_t *pcm, snd_pcm_hw_params_t *params,
		unsigned int val, int dir);

static const struct { const char *name; int id; } formats[] = {
	{ "S24_LE", 6 }, { "S32_LE", 10 }, { "DSD_U8", 48 },
	{ "DSD_U16_LE", 49 }, { "DSD_U32_LE", 50 }, { "DSD_U16_BE", 51 },
	{ "DSD_U32_BE", 52 },
};

static const unsigned int rates[] = {
	32000, 44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000,
	705600, 768000,
};

int main(int argc, char **argv)
{
	snd_pcm_t *pcm;
	char buf[4096];
	snd_pcm_hw_params_t *hw = (snd_pcm_hw_params_t *)buf;
	int i, f, id;
	unsigned int r;

	if (argc < 3 || snd_pcm_hw_params_sizeof() > sizeof(buf))
		return 2;
	if (snd_pcm_open(&pcm, argv[1], 0, 0) < 0) {
		printf("cannot open %s\n", argv[1]);
		return 1;
	}
	for (i = 2; i < argc; i++) {
		id = -1;
		for (f = 0; f < (int)(sizeof(formats) / sizeof(formats[0])); f++)
			if (!strcmp(argv[i], formats[f].name))
				id = formats[f].id;
		printf("%-10s:", argv[i]);
		memset(buf, 0, sizeof(buf));
		if (id < 0 || snd_pcm_hw_params_any(pcm, hw) < 0 ||
		    snd_pcm_hw_params_set_channels(pcm, hw, 2) < 0 ||
		    snd_pcm_hw_params_set_format(pcm, hw, id) < 0) {
			printf(" format refused\n");
			continue;
		}
		for (r = 0; r < sizeof(rates) / sizeof(rates[0]); r++)
			if (!snd_pcm_hw_params_test_rate(pcm, hw, rates[r], 0))
				printf(" %u", rates[r]);
		printf("\n");
	}
	snd_pcm_close(pcm);
	return 0;
}
