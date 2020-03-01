# mytekusb2

Linux ALSA USB driver for the Mytek Digital Stereo192-DSD DAC that WORKS with newer kernels.
Since the original [project](https://github.com/lintweaker/mytekusb2) by [lintweaker](https://github.com/lintweaker)
has apparently been abandoned, I've decided to fork it and apply two simple patches to make the driver work with current kernels.
It's been tested on Ubuntu 18.04 with 5.3.0-40-generic.

## Pulse audio stereo sink

Over the years the default settings of PulseAudio has changed and with current defaults Mytek is recognised as a mono sink.
To solve this issue you can:

- create an udev rule and Mytek specific PulseAudio profile (the hard, yet the proper way),

- edit the default PulseAudio configuration file and comment out the analog mono mapping, then Mytek will register with stereo-fallback profile (easy way, but be careful since no mono device will work properly with such profile).

Enjoy the music!
Mikołaj Molenda

>Linux ALSA USB driver for the Mytek Digital Stereo192-DSD DAC
>
>This is the Linux ALSA driver for the Mytek Digital Stereo192-DSD DAC using its
>USB2 interface. It is based on the TerraTec DMX 6Fire USB driver by Torsten Schenk.
>The driver has been tailored to work with the Mytek. All features of the original 
>driver not usable for the Mytek have been removed.
>
>Current features:
>- automatic firmware loading (see FIRMWARE and ISSUES)
>- playback at 24 and 32-bit, samplerates from 44.1k to 192.0k
>- This driver is tested with the Mytek DAC running firmware 1.7.1 and 1.7.5.5
>- Do not forget to switch the Mytek to 'USB2' input!
>
>Notes:
>- DoP (DSD over PCM) works using MPD 0.17 or newer and the latest squeezelite
>  versions
>- No mixer support as Mytek has no mixer controlable via USB
>
>Tested on:
>- Various x86 and x86_64 systems running recent versions of Fedora (>= 17)
>  including Fedora based Vortexbox 2.2
>- Tested on a ARM based Cubox running Fedora 17 hardfp
>- Tested on ARM with Wandboard Dual and Wandboard Quad running Community SqueezeOS
>
>Verify:
>- To verify that the driver is loaded and running properly you can check with
>  ALSA tool 'aplay'.
>
>Output for 'aplay -l' [Mytek is the second audio interface in this example]:
>
>card 1: USB2 [Mytek Stereo192-DSD USB2], device 0: MytekUSB2 [Mytek USB2]
>  Subdevices: 1/1
>  Subdevice #0: subdevice #0
>
>The driver needs three pieces of firmware to operate, see FIRMWARE for details.
>See ISSUES for current issues and INSTALL for installation guidelines.
>
>Enjoy!
>Jurgen Kramer