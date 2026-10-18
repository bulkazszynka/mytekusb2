# Testing without hardware

`run-qemu-test.sh` boots a test kernel in QEMU, attaches an emulated
Stereo192-DSD over USB/IP (vhci_hcd) and plays a list of test streams
through the driver with `aplay`.

* `mytek-emu.c` - the fake DAC, in its firmware-loaded state: same
  interfaces, alt settings and packet sizes as the real device, implicit
  feedback at the nominal rate. It checks every received frame: the test
  signal is a ramp (left = n, right = ~n), PCM words carry the 0x40 label,
  DoP frames carry alternating 0x05/0xfa markers. It also compares the
  register/GPIO sequence of each stream with the Windows driver's (from a
  USB capture). Fault injection: `-z N` zero-length feedback every N
  packets, `-e N` feedback packet errors, `-x N` OUT urbs completed with
  -EXDEV, `-d S` unplug after S seconds.
* `guest-init.sh` - the scenario list run inside the guest (all PCM rates,
  every DSD format at DSD64/DSD128, refused formats and rates, parameter
  variants, fault injection, unplug/replug, rmmod), then a kernel log
  check (`KERNEL-LOG-CLEAN`).
* `gen-ramp.py` - writes the ramp test signals; `alsa-rates.c` asks ALSA
  which rates each format accepts (`aplay -r` silently falls back to the
  nearest allowed rate, so it cannot show a refusal).

## Running

Build a test kernel (any recent tree; 7.3 was used) with
`x86_64_defconfig` plus `kernel-test.config` (ALSA, USB/IP vhci, KASAN,
lockdep, UBSAN):

    cd linux
    make O=/tmp/kb x86_64_defconfig
    scripts/kconfig/merge_config.sh -O /tmp/kb /tmp/kb/.config ../tests/kernel-test.config
    make O=/tmp/kb -j$(nproc) bzImage modules

Then, from the repository root (needs `qemu-system-x86_64`, `busybox`,
`aplay`, gcc with static libc, python3):

    tests/run-qemu-test.sh /tmp/kb /tmp/mytek-test

The guest's output is in `/tmp/mytek-test/qemu.log`. A good run has
`discontinuities=0 bad_hdr=0 bad_marker=0` on every `EMU-STATS` line,
`vendor_seq=OK` except for the `unmute=0` and unplug cases, and
`KERNEL-LOG-CLEAN` at the end.

## Caveats

* Without KVM, QEMU emulates the CPU (TCG) and runs the fake DAC at
  roughly 25-100 % of real time. Timing counters (`in_late`,
  `in_gap_max_us`) mean nothing here, and a random stream can end with an
  aplay I/O error when the emulation stalls longer than ALSA's write
  timeout; the data delivered up to then stays bit-exact.
* The guest uses 2 CPUs: with 4, the guest's timers stalled under TCG
  (RCU stall warnings, guest hangs), independent of the driver.
* Not covered: the firmware upload stages (needs the vendor firmware) and
  real xHCI scheduling. Anything audible must be checked on the real DAC.
