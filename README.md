# mytekusb2

Linux ALSA USB driver for the Mytek Digital Stereo192-DSD DAC (USB2 input).
Forked from [lintweaker/mytekusb2](https://github.com/lintweaker/mytekusb2)
by Jurgen Kramer, which is based on the in-kernel TerraTec DMX 6Fire USB
driver by Torsten Schenk.

Features: automatic firmware loading (three files in `/lib/firmware/mytek`,
see FIRMWARE), playback at 24/32 bit, 44.1-192 kHz, stereo; native DSD64 and
DSD128 (ALSA DSD formats, sent to the DAC as DoP, see below), and DoP DSD64
from players that make DoP themselves. The DAC has no USB-controllable volume. Remember to switch the
Mytek to its USB2 input. `aplay -l` should list
`card N: USB2 [Mytek Stereo192-DSD USB2], device 0: MytekUSB2 [Mytek USB2]`.

## Version 2.0 (2026): Linux 6.x / 7.x, DKMS, click fixes

Builds on Linux 6.x and 7.x (tested: 6.8, 6.14, 6.17, 7.0, 7.3-rc).

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

### DSD (2.2)
The DAC takes DSD over USB2 only as DoP on its PCM input; the Windows ASIO
driver sends exactly that (DSD64 as DoP at 176.4 kHz, DSD128 as DoP at
352.8 kHz - checked with a USB capture). 2.2 offers the native ALSA DSD
formats and packs them into DoP in the driver, so players can output DSD
natively (e.g. MPD `dop "no"`, `pw-dsdplay`):

| format | DSD64 | DSD128 |
|---|---|---|
| `DSD_U8` | 352800 | 705600 |
| `DSD_U16_LE`/`_BE` | 176400 | 352800 |
| `DSD_U32_LE`/`_BE` | 88200 | 176400 |

PCM stays at 44.1-192 kHz (the DAC's limit). A player that makes DoP itself
can send DSD64 as 176.4k PCM as before; for DSD128 as DoP it needs 352.8k
PCM, which is only accepted with `dop352=1` - never use that for real PCM.
DSD256 is not supported by the DAC.

### Module parameters (writable at runtime, used at the next stream start)
* `out_lead` (0-12, default 6): ms of playback queued ahead; 0 = 1.x behaviour
* `zlp_fill` (default 1): frames for an empty feedback packet:
  0 = none, 1 = nominal, 2 = 5 (1.x)
* `unmute_delay` (0-2000, default 700): ms between stream start and
  unmuting the DAC (GPIO 0), so the relock on rate changes is not heard;
  the device open/prepare waits for it, no audio is lost
* `mute_delay` (0-1000, default 20): ms the DAC is muted before a stream
  stops; `dop_exit` (0-1000, default 0): ms of PCM silence (still muted)
  after a DSD stream before it stops
* `dop_preroll` (default 1): what a DSD stream sends before the music:
  1 = DoP silence, 0 = PCM zeros (the DAC switches to DSD only when the
  music starts, as with the Windows driver)
* `unmute` (default 1): diagnostic, 0 keeps the DAC muted
* `legacy_ports` (default 0): up to 192k use the I2S port setup of driver
  <= 2.2.0 (ports 0-2, ports 1/2 as DSD ports) instead of the Windows
  driver's port 0 only
* `dsd` (default 1): offer the native DSD formats (read at open)
* `dop352` (default 0): accept 24/32 bit PCM at 352.8 kHz, only for
  player-made DoP DSD128 (read at open)
* `comm_log` (default 0): log every register/GPIO message (diagnostics)

e.g. `echo 8 | sudo tee /sys/module/snd_usb_mytek/parameters/out_lead`,
or permanently in `/etc/modprobe.d/mytek.conf`:
`options snd-usb-mytek out_lead=8`

### Diagnostics
`cat /proc/asound/USB2/mytek` shows live streaming counters. When a stream
stops, a `stream stats:` line is written to the kernel log only if something
went wrong or the host came close to the `out_lead` headroom
(`journalctl -k | grep "stream stats"`). For the per-stream details of every
stream (rate/packet sizes, summaries) enable debug messages:
`echo 'module snd_usb_mytek +p' | sudo tee /sys/kernel/debug/dynamic_debug/control`.
`out_pkt_err` > 0 means packets the USB controller could not send in time
(audible gap); `in_late`/`in_gap_max_us` show host latency.

### If clicks return

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
     (power saving, a misbehaving driver); try `performance` CPU governor
     to confirm.
   * `empty`, `pkt_err`, `clamped` > 0: the DAC sent unusual feedback.
     Note the values; `zlp_fill=0/1/2` changes how empty packets are handled.
   * All counters 0 but you heard a click: the driver delivered every
     sample; check PipeWire with `pw-top` (ERR column = PipeWire underruns,
     fix by raising the quantum, e.g. `pw-metadata -n settings 0
     clock.force-quantum 2048`).
   * The click comes exactly when playback starts or the sample rate
     changes: that is the DAC re-locking its
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
