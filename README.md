# mytekusb2

Linux ALSA driver (`snd-usb-mytek`) for the **Mytek Digital Stereo192-DSD
DAC** on its USB2 input, for Linux 6.x and 7.x.

* PCM 44.1-192 kHz, 24/32 bit, stereo
* native DSD64 and DSD128 (ALSA DSD formats, sent to the DAC as DoP)
* DoP from players that make DoP themselves
* DKMS packaging: rebuilt automatically on kernel updates, signed for
  Secure Boot
* firmware upload on every power-on (the Mytek firmware files are needed,
  see [FIRMWARE](FIRMWARE))

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
MOK. On Ubuntu, DKMS signs with `/var/lib/shim-signed/mok/MOK.priv`; if that
key is not enrolled yet, run `sudo update-secureboot-policy --enroll-key`
and reboot (or point `mok_signing_key`/`mok_certificate` in
`/etc/dkms/framework.conf` to your own enrolled key). `dkms-install.sh`
warns if the installed module is not signed.

## Playback

### PCM
44.1, 48, 88.2, 96, 176.4 and 192 kHz, `S24_LE` or `S32_LE`, 2 channels.
The DAC has no USB-controllable volume. PCM above 192 kHz is not offered:
the DAC cannot play it.

### DSD
The DAC takes DSD over USB2 only as DoP on its PCM input; the Windows ASIO
driver sends exactly that (DSD64 as DoP at 176.4 kHz, DSD128 as DoP at
352.8 kHz - checked with a USB capture). The driver offers the native ALSA
DSD formats and packs them into DoP itself, so players can output DSD
natively (e.g. MPD `dop "no"`, `pw-dsdplay`):

| format | DSD64 | DSD128 |
|---|---|---|
| `DSD_U8` | 352800 | 705600 |
| `DSD_U16_LE`/`_BE` | 176400 | 352800 |
| `DSD_U32_LE`/`_BE` | 88200 | 176400 |

A quick test with a raw DSD64 file (MSB first, 32 bit per channel word):

    aplay -D hw:USB2 -t raw -c 2 -f DSD_U32_BE -r 88200 file.raw

A player that makes DoP itself can send DSD64 as 176.4 kHz PCM; DSD128 as
DoP needs 352.8 kHz PCM, which is only accepted with `dop352=1` - never use
that for real PCM. DSD256 is not supported by the DAC.

The display shows DSD / HDSD for DSD64 / DSD128. The DAC clicks when it
switches between PCM and DSD (see [ISSUES](ISSUES)); gapless playback of
tracks with the same DSD rate does not switch.

## Module parameters
Writable at runtime (`echo 8 | sudo tee
/sys/module/snd_usb_mytek/parameters/out_lead`), used from the next stream
on; permanently in `/etc/modprobe.d/mytek.conf`, e.g.
`options snd-usb-mytek out_lead=8`.

Streaming:
* `out_lead` (0-12, default 6): ms of playback queued ahead of the bus;
  more tolerates more host latency (0 = driver 1.x behaviour)
* `zlp_fill` (0/1/2, default 1): frames sent for a feedback packet without
  data: 0 = none, 1 = nominal for the rate, 2 = always 5 (1.x)
* `unmute_delay` (0-2000, default 700): ms of silence after a stream
  starts, while the DAC relocks to the new rate; opening/preparing the
  device waits for it, no audio is lost

DSD:
* `dsd` (default 1): offer the native DSD formats (read at open)
* `dop_preroll` (default 0): what a DSD stream sends before the music:
  0 = PCM zeros (the DAC switches to DSD only when the music starts, as
  with the Windows driver), 1 = DoP silence
* `dop_exit` (0-5000, default 200): ms of PCM silence after a DSD stream
  before it stops, so the DAC leaves DSD mode while its clock still runs
  (without it the DAC clicks at DSD64 stops and keeps showing "DSD" for
  later PCM)
* `dop352` (default 0): accept 24/32 bit PCM at 352.8 kHz, only for
  player-made DoP DSD128 (read at open)

Diagnostics:
* `mute_delay` (0-1000, default 20): ms between the Windows driver's
  "mute" sequence (GPIO 0 low, FPGA reg 6 = 0) and the stream stop; the
  driver copies the vendor sequence, it has no audible effect on this DAC
* `unmute` (default 1): 0 = never send the vendor "unmute"
* `legacy_ports` (default 0): up to 192 kHz use the I2S port setup of
  driver <= 2.2.0 (ports 0-2) instead of the Windows driver's port 0 only
* `comm_log` (default 0): log every register/GPIO message

## Troubleshooting

### No card / firmware problems
See [ISSUES](ISSUES): after power-on an upload stage can fail - reload the
module (`sudo modprobe -r snd_usb_mytek && sudo modprobe snd_usb_mytek`) or
use `scripts/mytekusb2-firmware-loader.sh`.

### Diagnostics
`cat /proc/asound/USB2/mytek` shows live streaming counters. When a stream
stops, a `stream stats:` line is written to the kernel log only if something
went wrong or the host came close to the `out_lead` headroom
(`journalctl -k | grep "stream stats"`). For the details of every stream
enable debug messages:
`echo 'module snd_usb_mytek +p' | sudo tee /sys/kernel/debug/dynamic_debug/control`
(with Secure Boot lockdown debugfs is blocked; `comm_log=1` logs the
register messages without it).

### Clicks during playback
Clicks when DSD starts or stops come from the DAC (see above). For clicks
*during* playback:

1. Note the time of the click, then look at what the driver measured (a
   line is logged for every stream that had problems or a close call, the
   newest last; no line = the driver saw nothing wrong):

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
     (power saving, a misbehaving driver); try the `performance` CPU
     governor to confirm.
   * `empty`, `pkt_err`, `clamped` > 0: the DAC sent unusual feedback.
     Note the values; `zlp_fill=0/1/2` changes how empty packets are handled.
   * All counters 0 but you heard a click: the driver delivered every
     sample; check PipeWire with `pw-top` (ERR column = PipeWire underruns,
     fix by raising the quantum, e.g. `pw-metadata -n settings 0
     clock.force-quantum 2048`).

3. Every stream start waits `unmute_delay` (0.7 s) for the DAC. To avoid
   that on every sound after a pause, stop PipeWire from suspending the DAC
   (WirePlumber `session.suspend-timeout-seconds = 0`).

4. If the driver is missing after a kernel update: `dkms status` should list
   `snd-usb-mytek` for the new kernel; if not, install the headers
   (`sudo apt install linux-headers-$(uname -r)`) and run
   `sudo ./scripts/dkms-install.sh` again.

## Testing without hardware
`tests/` contains a USB/IP emulation of the DAC that verifies every frame
and a QEMU harness, see [tests/README.md](tests/README.md).

## Changes

### 2.2 (2026): native DSD
* Native DSD64/DSD128 through the ALSA DSD formats, packed into DoP in the
  driver; DSD128 uses the DAC's 352.8 kHz DoP carrier (alt setting 4).
* I2S port setup, idle alt setting and mute/unmute sequence as the Windows
  driver does them (from a USB capture); PCM limited to the DAC's 192 kHz.
* Silent pre-roll after every stream start (`unmute_delay`): no more clicks
  on PCM rate changes. DSD start/stop like the Windows driver
  (`dop_preroll`, `dop_exit`): no clicks at DSD64 stops, and the display no
  longer stays at "DSD" during PCM.
* FPGA upload padded like the vendor drivers.

### 2.0 (2026): Linux 6.x / 7.x, DKMS, click fixes
* Builds on current kernels (removed APIs replaced), DKMS packaging.
* Playback is queued `out_lead` ms ahead of the bus (default 6). 1.x had
  ~1 ms of headroom, so any interrupt latency caused gaps - the "static
  clicks" of 1.x.
* Feedback packets without data are filled with the nominal frame count for
  the sample rate (1.x always used 5 frames, wrong above 44.1 kHz).
* Packet sizes bounded, no permanent stop after one bad USB packet, sample
  rate changes without closing the device, stereo only (no mono sink), no
  slot leak across firmware-upload reconnects, no use-after-free on unplug.

## License
GPL version 2 or later, see [LICENSE](LICENSE) (GPL-2.0 text).
