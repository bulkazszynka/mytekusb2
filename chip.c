/*
 * Linux driver for Mytek Digital Stereo192-DSD DAC USB2
 *
 * Main routines and module definitions.
 *
 * Based on 6fire usb driver
 *
 * Adapted for Mytek by	: Jurgen Kramer
 * Last updated		: Oct 15, 2026 (Linux 6.x/7.x port)
 * Copyright		: (C) Jurgen Kramer
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "chip.h"
#include "firmware.h"
#include "pcm.h"
#include "control.h"
#include "comm.h"

#include <linux/moduleparam.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/gfp.h>
#include <sound/initval.h>

MODULE_AUTHOR("Jurgen Kramer <gtmkramer@xs4all.nl>");
MODULE_DESCRIPTION("Mytek Digital Stereo192-DSD DAC USB2 audio driver");
MODULE_LICENSE("GPL v2");
MODULE_VERSION(MYTEK_DRIVER_VERSION);

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX; /* Index 0-max */
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR; /* Id for card */
static bool enable[SNDRV_CARDS] = SNDRV_DEFAULT_ENABLE_PNP; /* Enable card */
static struct mytek_chip *chips[SNDRV_CARDS] = SNDRV_DEFAULT_PTR;

module_param_array(index, int, NULL, 0444);
MODULE_PARM_DESC(index, "Index value for the mytek sound device");
module_param_array(id, charp, NULL, 0444);
MODULE_PARM_DESC(id, "ID string for the mytek sound device.");
module_param_array(enable, bool, NULL, 0444);
MODULE_PARM_DESC(enable, "Enable the mytek sound device.");

/* protects chips[] and covers the whole probe and disconnect paths */
static DEFINE_MUTEX(register_mutex);

static void mytek_chip_abort(struct mytek_chip *chip)
{
	if (chip->pcm)
		mytek_pcm_abort(chip);
	if (chip->comm)
		mytek_comm_abort(chip);
	if (chip->control)
		mytek_control_abort(chip);
}

/* called when the last reference to the card is dropped */
static void mytek_card_free(struct snd_card *card)
{
	struct mytek_chip *chip = card->private_data;

	if (chip->pcm)
		mytek_pcm_destroy(chip);
	if (chip->control)
		mytek_control_destroy(chip);
	if (chip->comm)
		mytek_comm_destroy(chip);
}

static int mytek_chip_probe(struct usb_interface *intf,
		const struct usb_device_id *usb_id)
{
	int ret;
	int i;
	struct mytek_chip *chip = NULL;
	struct usb_device *device = interface_to_usbdev(intf);
	int regidx = -1; /* index in module parameter array */
	struct snd_card *card = NULL;

	mutex_lock(&register_mutex);

	/* look if we already serve this card and return if so */
	for (i = 0; i < SNDRV_CARDS; i++) {
		if (chips[i] && chips[i]->dev == device) {
			chips[i]->intf_count++;
			usb_set_intfdata(intf, chips[i]);
			ret = 0;
			goto unlock;
		} else if (!chips[i] && regidx < 0)
			regidx = i;
	}
	if (regidx < 0) {
		dev_err(&intf->dev, "too many cards registered.\n");
		ret = -ENODEV;
		goto unlock;
	}
	if (!enable[regidx]) {
		ret = -ENOENT;
		goto unlock;
	}

	/* check, if firmware is present on device, upload it if not */
	ret = mytek_fw_init(intf);
	if (ret < 0)
		goto unlock;
	if (ret == FW_NOT_READY) { /* firmware update performed */
		ret = 0;
		goto unlock;
	}

	/* if we are here, card can be registered in alsa. */
	if (usb_set_interface(device, 0, 0) != 0) {
		dev_err(&intf->dev, "cannot set first interface.\n");
		ret = -EIO;
		goto unlock;
	}
	ret = snd_card_new(&intf->dev, index[regidx], id[regidx], THIS_MODULE,
			sizeof(struct mytek_chip), &card);
	if (ret < 0) {
		dev_err(&intf->dev, "cannot create alsa card.\n");
		goto unlock;
	}
	strscpy(card->driver, "MytekUSB", sizeof(card->driver));
	strscpy(card->shortname, "Mytek Stereo192-DSD USB2",
		sizeof(card->shortname));
	snprintf(card->longname, sizeof(card->longname), "%s at %d:%d",
		 card->shortname, device->bus->busnum, device->devnum);

	chip = card->private_data;
	chip->dev = device;
	chip->regidx = regidx;
	chip->intf_count = 1;
	chip->card = card;
	card->private_free = mytek_card_free;

	ret = mytek_comm_init(chip);
	if (ret < 0)
		goto destroy_chip;

	ret = mytek_pcm_init(chip);
	if (ret < 0)
		goto destroy_chip;

	ret = mytek_control_init(chip);
	if (ret < 0)
		goto destroy_chip;

	ret = snd_card_register(card);
	if (ret < 0) {
		dev_err(&intf->dev, "cannot register card.\n");
		goto destroy_chip;
	}

	usb_set_intfdata(intf, chip);
	chips[regidx] = chip;
	dev_info(&intf->dev, "snd-usb-mytek %s ready.\n", MYTEK_DRIVER_VERSION);
	ret = 0;
	goto unlock;

destroy_chip:
	chip->shutdown = true;
	snd_card_disconnect(card);
	mytek_chip_abort(chip);
	snd_card_free(card);
unlock:
	mutex_unlock(&register_mutex);
	return ret;
}

static void mytek_chip_disconnect(struct usb_interface *intf)
{
	struct mytek_chip *chip;
	struct snd_card *card;

	mutex_lock(&register_mutex);
	chip = usb_get_intfdata(intf);
	/* if !chip, fw upload has been performed */
	if (!chip)
		goto unlock;

	chip->intf_count--;
	if (chip->intf_count)
		goto unlock;

	chips[chip->regidx] = NULL;

	/*
	 * snd_card_free_when_closed() may free the card (and the chip
	 * embedded in it) immediately, so it must be called last.
	 */
	card = chip->card;
	chip->shutdown = true;
	snd_card_disconnect(card);
	mytek_chip_abort(chip);
	snd_card_free_when_closed(card);
unlock:
	mutex_unlock(&register_mutex);
}

static const struct usb_device_id device_table[] = {
	{
		.match_flags = USB_DEVICE_ID_MATCH_DEVICE,
		.idVendor = 0x25ce,
		.idProduct = 0x000e
	},
	{
		.match_flags = USB_DEVICE_ID_MATCH_DEVICE,
		.idVendor = 0x25ce,
		.idProduct = 0x0010
	},
	{}
};

MODULE_DEVICE_TABLE(usb, device_table);

static struct usb_driver usb_driver = {
	.name = "snd-usb-mytek",
	.probe = mytek_chip_probe,
	.disconnect = mytek_chip_disconnect,
	.id_table = device_table,
};

module_usb_driver(usb_driver);
