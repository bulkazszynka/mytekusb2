/*
 * Linux driver for Mytek Digital Stereo192-DSD DAC USB2
 *
 * Mixer control
 *
 * Based on 6fire usb driver
 *
 * Adapted for Mytek by	: Jurgen Kramer
 * Last updated		: Feb 28, 2014
 * Copyright		: (C) Jurgen Kramer
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/interrupt.h>
#include <linux/delay.h>
#include <sound/control.h>
#include <sound/tlv.h>

#include "control.h"
#include "comm.h"
#include "chip.h"

/*
 * Init data that needs to be sent to device.
 * Taken from snooping the WinXP USB driver
 */

static const struct {
	u8 type;
	u8 reg;
	u8 valh;
	u8 vall;
}

init_data_mytek[] = {

	{ 0x02, 0x09, 0x00, 0x01 },	/* Set mystery reg 9 to 1 */
	{ 0x22, 0x00, 0x00, 0xff },	/* Set Pin=0 to low */
	{ 0x20, 0x00, 0x08, 0xff },	/* Pin PC0 to output */
	{ 0x22, 0x03, 0x00, 0xff },	/* Set Pin=3 to low */
	{ 0x20, 0x03, 0x08, 0xff },	/* Set GPIO pin PC3 to output */
	{ 0x02, 0x01, 0x00, 0x10 },	/* Set rate = 88.2k */
	{ 0x02, 0x81, 0x00, 0x00 },	/* Read rate */
	{ 0x02, 0x00, 0x00, 0x00 },	/* Disable streaming */
	{ 0x02, 0x80, 0x00, 0x00 },	/* Check it */
	{ 0x02, 0x05, 0x00, 0x00 },	/* Out Sample Delay = 0 */
	{ 0x02, 0x02, 0x07, 0x07 },	/* Enable all I2S_I and I2S_O */
	{ 0x02, 0x04, 0x00, 0x06 },	/* Set DSD_IO[7..0] -> 6 = 0000 0110 */
	{ 0x02, 0x03, 0x00, 0x00 },	/* Disable SPDIF_I and SPDIF_O */
	{ 0x02, 0x09, 0x00, 0x01 },	/* Set mystery reg 9 to 1 */
	{ 0x02, 0x00, 0x00, 0x01 },	/* Enable streaming */
	{ }
};

/*
 * Per-rate device setup. CLK_SEL (FPGA reg 1) codes and alt settings are
 * those of the vendor driver's rate table. Up to 192k the I2S port setup is
 * the one this driver always used (out ports 0-2, in ports 0-1). 352.8k
 * only fits into a packet with port 0 alone, which is what the Windows
 * driver uses for every rate (USB capture, 2026-10-18): EN_I2S 0x0101,
 * EN_DSD 0. The DAC takes 352.8k only as the DoP carrier for DSD128 (its
 * PCM limit is 192k); the vendor's 384k entry (0x0107) is left out.
 * Keep synced with CONTROL_RATE_XXX in control.h and the tables in pcm.c.
 */
static const struct {
	u8 alt;
	u16 clk_sel;	/* reg 1 */
	u16 en_i2s;	/* reg 2: out port mask << 8 | in port mask */
	u16 en_dsd;	/* reg 4 */
} rate_setup[CONTROL_N_RATES] = {
	{ 1, 0x0011, 0x0703, 0x0006 },	/* 44.1k */
	{ 1, 0x0111, 0x0703, 0x0006 },	/* 48k */
	{ 2, 0x0010, 0x0703, 0x0006 },	/* 88.2k */
	{ 2, 0x0110, 0x0703, 0x0006 },	/* 96k */
	{ 3, 0x0000, 0x0703, 0x0006 },	/* 176.4k */
	{ 3, 0x0100, 0x0703, 0x0006 },	/* 192k */
	{ 4, 0x0007, 0x0101, 0x0000 },	/* 352.8k, DoP DSD128 */
};

/* write16() takes the high byte first */
static int mytek_control_fpga_write(struct comm_runtime *comm_rt, u8 reg,
		u16 value)
{
	return comm_rt->write16(comm_rt, 0x02, reg, value >> 8, value & 0xff);
}

static int mytek_control_set_rate(struct control_runtime *rt, int rate)
{
	int ret;
	struct usb_device *device = rt->chip->dev;
	struct comm_runtime *comm_rt = rt->chip->comm;

	if (rate < 0 || rate >= CONTROL_N_RATES)
		return -EINVAL;

	ret = usb_set_interface(device, 1, rate_setup[rate].alt);
	if (ret < 0) {
		dev_err(&rt->chip->dev->dev,
			"mytek_control_set_rate: usb_set_interface failed\n");
		return ret;
	}

	comm_rt->write8(comm_rt, 0x22, 0x00, 0x00);		/* Set Pin 0 to low state (mute) */
	comm_rt->write16(comm_rt, 0x02, 0x00, 0x00, 0x00);	/* Disable streaming */

	/* set soundcard clock */
	ret = mytek_control_fpga_write(comm_rt, 0x01, rate_setup[rate].clk_sel);
	if (ret < 0) {
		dev_err(&rt->chip->dev->dev,
			"mytek_control_set_rate: setting rate failed\n");
		return ret;
	}

	comm_rt->write16(comm_rt, 0x02, 0x00, 0x00, 0x00);	/* Disable streaming */
	comm_rt->write16(comm_rt, 0x02, 0x05, 0x00, 0x00);	/* Set out sample delay to 0 */
	mytek_control_fpga_write(comm_rt, 0x02, rate_setup[rate].en_i2s);
	mytek_control_fpga_write(comm_rt, 0x04, rate_setup[rate].en_dsd);
	comm_rt->write16(comm_rt, 0x02, 0x03, 0x00, 0x00);	/* Disable SPDIF O and I */
	comm_rt->write16(comm_rt, 0x02, 0x09, 0x00, 0x01);	/* Set mystery reg 9 to 1 */

	msleep(15);		/* Let the Mytek's display catch up with the new rate */

	return 0;
}

/*
 * GPIO pin 0 and FPGA reg 6 bit 1 as the Windows driver drives them (USB
 * capture): both set some time after streaming started, both cleared
 * before streaming stops. Pin 0 low = output muted, so the DAC does not
 * play the relock on rate and PCM/DSD changes.
 */
static int mytek_control_set_mute(struct control_runtime *rt, bool mute)
{
	struct comm_runtime *comm_rt = rt->chip->comm;
	int ret;

	ret = comm_rt->write8(comm_rt, 0x22, 0x00, mute ? 0x00 : 0x01);
	if (ret < 0)
		return ret;
	return mytek_control_fpga_write(comm_rt, 0x06, mute ? 0x0000 : 0x0002);
}

static int mytek_control_streaming_update(struct control_runtime *rt)
{
	struct comm_runtime *comm_rt = rt->chip->comm;

	if (comm_rt) {

		return comm_rt->write16(comm_rt, 0x02, 0x00, 0x00,
			(rt->usb_streaming ? 0x01 : 0x00) );

	}
	return -EINVAL;
}

int mytek_control_init(struct mytek_chip *chip)
{
	int i;
	struct control_runtime *rt = kzalloc(sizeof(struct control_runtime),
			GFP_KERNEL);
	struct comm_runtime *comm_rt = chip->comm;

	if (!rt)
		return -ENOMEM;

	rt->chip = chip;
	rt->update_streaming = mytek_control_streaming_update;
	rt->set_rate = mytek_control_set_rate;
	rt->set_mute = mytek_control_set_mute;

	i = 0;

	while (init_data_mytek[i].type) {

		if (init_data_mytek[i].type != 0x02) {
			comm_rt->write8(comm_rt, init_data_mytek[i].type,
					init_data_mytek[i].reg,
					init_data_mytek[i].valh);
		} else {
			comm_rt->write16(comm_rt, init_data_mytek[i].type,
					 init_data_mytek[i].reg,
					 init_data_mytek[i].valh,
					 init_data_mytek[i].vall);
		}	
		i++;
	}

	mytek_control_streaming_update(rt);
	chip->control = rt;

	return 0;
}

void mytek_control_abort(struct mytek_chip *chip)
{
}

void mytek_control_destroy(struct mytek_chip *chip)
{
	kfree(chip->control);
	chip->control = NULL;
}
