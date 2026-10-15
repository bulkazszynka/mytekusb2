# mytekusb2

Linux ALSA USB driver for the Mytek Digital Stereo192-DSD DAC that WORKS with newer kernels.
Since the original [project](https://github.com/lintweaker/mytekusb2) by [lintweaker](https://github.com/lintweaker)
has apparently been abandoned, I've decided to fork it and apply two simple patches to make the driver work with current kernels.
It's been tested on Ubuntu 18.04 with 5.3.0-40-generic.

## Version 2.0 (2026): Linux 6.x / 7.x, DKMS, click fixes

Builds on Linux 6.x and 7.x (tested: 6.8, 6.14, 7.0, 7.3-rc).

### Install with DKMS (rebuilt automatically on kernel updates)

    sudo apt install dkms build-essential linux-headers-$(uname -r)
    sudo ./scripts/dkms-install.sh            # install / upgrade
    sudo ./scripts/dkms-install.sh --remove   # uninstall

Firmware is still needed in `/lib/firmware/mytek` (see FIRMWARE).

### Secure Boot
With Secure Boot enabled the module must be signed with a key enrolled in
MOK. On Ubuntu, DKMS signs with `/var/lib/shim-signed/mok/MOK.priv`; if that
key is not enrolled yet, run `sudo update-secureboot-policy --enroll-key`
and reboot (or point `mok_signing_key`/`mok_certificate` in
`/etc/dkms/framework.conf` to your own enrolled key).

### What changed against 1.x
* Playback is queued `out_lead` ms ahead of the bus (default 6). 1.x had
  ~1 ms of headroom, so any interrupt latency caused gaps (clicks).
* Feedback packets without data are filled with the nominal frame count for
  the sample rate (1.x always used 5 frames, wrong above 44.1 kHz).
* Packet sizes bounded, no permanent stop after one bad USB packet, sample
  rate changes without closing the device, stereo only (no mono sink),
  no slot leak across firmware-upload reconnects, no use-after-free on
  unplug.

### Module parameters (writable at runtime, used at the next stream start)
* `out_lead` (0-12, default 6): ms of playback queued ahead; 0 = 1.x behaviour
* `zlp_fill` (default 1): frames for an empty feedback packet:
  0 = none, 1 = nominal, 2 = 5 (1.x)

e.g. `echo 8 | sudo tee /sys/module/snd_usb_mytek/parameters/out_lead`,
or permanently in `/etc/modprobe.d/mytek.conf`:
`options snd-usb-mytek out_lead=8`

### Diagnostics
`cat /proc/asound/USB2/mytek` shows live streaming counters; each stream
stop logs a `stream stats:` line (`journalctl -k | grep "stream stats"`).
`out_pkt_err` > 0 means packets the USB controller could not send in time
(audible gap); `in_late`/`in_gap_max_us` show host latency.

### If clicks return

1. Note the time of the click, then look at what the driver measured (one
   line per playback session, the newest last):

       journalctl -k --since today | grep -E "stream stats|out packets|in packet|starved"
       cat /proc/asound/USB2/mytek      # live counters of the running stream

2. Read the line:
   * `out_pkt_err` > 0 or "out packets not sent": the USB controller ran out
     of queued audio -> audible gap. Compare `gap_max` with `lead` (ms).
     If `gap_max` gets close to `lead` (e.g. 5000us with lead=6), give the
     driver more headroom (adds that many ms of latency, harmless for music):

         echo "options snd-usb-mytek out_lead=10" | sudo tee /etc/modprobe.d/mytek.conf

     It takes effect on the next stream (or now:
     `echo 10 | sudo tee /sys/module/snd_usb_mytek/parameters/out_lead`).
     Maximum is 12. Large `gap_max` also suggests a system problem
     (power saving, a misbehaving driver); try `performance` CPU governor
     to confirm.
   * `empty`, `pkt_err`, `clamped` > 0: the DAC sent unusual feedback.
     Note the values; `zlp_fill=0/1/2` changes how empty packets are handled.
   * All counters 0 but you heard a click: the driver delivered every
     sample; check PipeWire with `pw-top` (ERR column = PipeWire underruns,
     fix by raising the quantum, e.g. `pw-metadata -n settings 0
     clock.force-quantum 2048`).
   * The click comes exactly when playback starts or the sample rate
     changes (a new "rate ..." line): that is the DAC re-locking its
     clock, expected. To avoid most of them, stop PipeWire from suspending
     the DAC and switching rates (WirePlumber `session.suspend-timeout-seconds = 0`,
     fixed `default.clock.rate`).

3. If the driver is missing after a kernel update: `dkms status` should list
   `snd-usb-mytek` for the new kernel; if not, install the headers
   (`sudo apt install linux-headers-$(uname -r)`) and run
   `sudo ./scripts/dkms-install.sh` again.

### Testing without hardware
`tests/` contains a USB/IP emulation of the DAC and a QEMU harness
(`tests/run-qemu-test.sh`).

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