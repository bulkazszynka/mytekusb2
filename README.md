# mytekusb2

Linux ALSA driver (`snd-usb-mytek`) for the **Mytek Digital Stereo192-DSD
DAC** on its USB2 input, for Linux 6.x and 7.x.

* PCM 44.1-192 kHz, 24/32 bit, stereo
* native DSD64 and DSD128
* DKMS packaging: rebuilt automatically on kernel updates, signed for
  Secure Boot
* needs the Mytek firmware files, see [FIRMWARE](FIRMWARE)

Tested on the real DAC with Ubuntu kernel 6.17; builds on 6.8, 6.14, 6.17,
7.0 and 7.3.

Forked from [lintweaker/mytekusb2](https://github.com/lintweaker/mytekusb2)
by Jurgen Kramer, which is based on the in-kernel TerraTec DMX 6Fire USB
driver by Torsten Schenk.

## Installation

1. Put the three Mytek firmware files into `/lib/firmware/mytek` - they come
   with Mytek's Windows/macOS USB2 driver, see [FIRMWARE](FIRMWARE).
2. Install the module with DKMS:

       sudo apt install dkms build-essential linux-headers-$(uname -r)
       sudo ./scripts/dkms-install.sh            # install / upgrade
       sudo ./scripts/dkms-install.sh --remove   # uninstall

   (other distributions and a plain `make install`: [INSTALL](INSTALL)).
3. Switch the Mytek to its **USB2** input. `aplay -l` should list

       card N: USB2 [Mytek Stereo192-DSD USB2], device 0: MytekUSB2 [Mytek USB2]

### Secure Boot
With Secure Boot enabled the module must be signed with a key enrolled in
MOK. On Ubuntu, DKMS does this automatically; if its key is not enrolled
yet, run `sudo update-secureboot-policy --enroll-key` and reboot (to use
your own key, set `mok_signing_key`/`mok_certificate` in
`/etc/dkms/framework.conf`). `dkms-install.sh` warns if the installed module is not signed.

## Playback

### PCM
44.1, 48, 88.2, 96, 176.4 and 192 kHz, 24 or 32 bit, stereo. The DAC has
no USB-controllable volume.

### DSD
Players can send DSD64 and DSD128 natively (e.g. MPD with `dop "no"`); the
driver converts it to the DoP stream the DAC expects. Players that produce
DoP themselves work too for DSD64. DSD256 is not supported by the DAC.

The display shows DSD / HDSD for DSD64 / DSD128. The DAC may click when it
switches between PCM and DSD (see [ISSUES](ISSUES)); gapless playback of
tracks with the same DSD rate does not switch.

## Module parameters
The defaults should work. If needed, set them in
`/etc/modprobe.d/mytek.conf`, e.g. `options snd-usb-mytek out_lead=8`.

* `out_lead` (0-12, default 6): ms of audio buffered ahead; raise it if
  you hear clicks during playback
* `unmute_delay` (0-2000, default 700): ms of silence at every stream
  start while the DAC locks to the new rate (prevents clicks on rate
  changes)
* `dsd` (default 1): offer native DSD formats
* `dop_preroll`, `dop_exit`: how DSD streams start and end (tuned to
  minimise the DAC's PCM/DSD switching clicks)
* `dop352` (default 0): for players that make DoP DSD128 themselves
  (352.8 kHz carrier); never use it for real PCM

`modinfo snd-usb-mytek` lists all parameters, including diagnostic ones.

## Troubleshooting

* **No sound card / firmware problems**: see [ISSUES](ISSUES). Reloading
  the module usually helps:
  `sudo modprobe -r snd_usb_mytek && sudo modprobe snd_usb_mytek`.
* **Clicks during playback**: check `journalctl -k | grep "stream stats"` -
  the driver logs a line when a stream had problems. If it reports
  "out packets not sent", raise `out_lead` (e.g. to 10). If the driver
  reports nothing, the cause is likely in the sound server (for PipeWire
  check `pw-top` for errors and try a larger quantum).
* **Short delay before every sound**: each stream start waits
  `unmute_delay`; stop PipeWire from suspending the DAC (WirePlumber
  `session.suspend-timeout-seconds = 0`).
* **Driver missing after a kernel update**: `dkms status` should list
  `snd-usb-mytek` for the new kernel; if not, install the matching headers
  and run `sudo ./scripts/dkms-install.sh` again.

## Testing without hardware
`tests/` contains an emulated DAC and a QEMU harness, see
[tests/README.md](tests/README.md).

## Changes

### 2.2 (2026)
* Native DSD64/DSD128.
* No more clicks on sample rate changes; fewer clicks at DSD start/stop.

### 2.0 (2026)
* Works on Linux 6.x / 7.x, DKMS packaging.
* Fixes the "static clicks" of 1.x (too little buffering).
* Stability fixes (unplug, reconnects, bad USB packets).

## License
GPL version 2 or later, see [LICENSE](LICENSE) (GPL-2.0 text).
