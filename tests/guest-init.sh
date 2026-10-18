#!/bin/sh
# Runs as /init inside the QEMU test guest.
mount -t proc proc /proc; mount -t sysfs sys /sys; mount -t devtmpfs dev /dev
mkdir -p /tmp; mount -t tmpfs tmp /tmp
export LD_LIBRARY_PATH=/lib/x86_64-linux-gnu:/usr/lib/x86_64-linux-gnu
log() { echo "=== TEST: $*"; }
emu_stats() { kill -USR1 $EMU; sleep 0.5; }
wait_card() {
	for i in $(seq 1 50); do [ -e /proc/asound/card0/mytek ] && return 0; sleep 0.2; done
	echo "card did not appear"; return 1
}
play() { # fmt rate file [extra aplay args]
	log "play $1 @ $2"
	aplay -q -D hw:0,0 -f $1 -r $2 -c 2 -t raw --buffer-size=$2 $4 /data/$3 ; echo "aplay exit=$?"
	grep -v -E '^(driver_version|state|zlp_fill|out_lead|in_packet_size|out_packet_size|packets_per_sec):' /proc/asound/card0/mytek | tr '\n' ' '; echo
	dmesg -c | tee -a /tmp/klog | grep -v -E 'callbacks suppressed' | tail -8
	emu_stats
}

insmod /snd-usb-mytek.ko || echo "insmod failed"
dmesg -n 4
dmesg -c > /tmp/klog

log "basic: attach emulated device"
/mytek-emu & EMU=$!
wait_card && cat /proc/asound/cards
play S32_LE 48000 s32_48k.raw
play S24_LE 44100 s24_44k.raw
play S32_LE 96000 s32_96k.raw
play S32_LE 192000 s32_192k.raw
play S24_LE 176400 s24_176k.raw
log "mono must be refused"
aplay -q -D hw:0,0 -f S32_LE -r 48000 -c 1 -t raw -d 1 /dev/zero; echo "aplay mono exit=$?"
log "pause/resume and small period"
aplay -q -D hw:0,0 -f S32_LE -r 48000 -c 2 -t raw --period-size=600 --buffer-size=4800 /data/s32_48k.raw; echo "aplay exit=$?"
emu_stats

# accepted rates per format (aplay -r would silently fall back to the
# nearest allowed rate, so refusals are checked with tests/alsa-rates.c)
log "rates per format: PCM must end at 192k (DAC limit); S32 352.8k only with dop352=1"
/alsa-rates hw:0,0 S24_LE S32_LE DSD_U8 DSD_U16_LE DSD_U16_BE DSD_U32_LE DSD_U32_BE
echo 1 > /sys/module/snd_usb_mytek/parameters/dop352
log "dop352=1"
/alsa-rates hw:0,0 S24_LE S32_LE
echo 0 > /sys/module/snd_usb_mytek/parameters/dop352
log "dsd=0"
echo 0 > /sys/module/snd_usb_mytek/parameters/dsd
/alsa-rates hw:0,0 S32_LE DSD_U8 DSD_U32_BE
echo 1 > /sys/module/snd_usb_mytek/parameters/dsd
log "dop352=1: S32 at 352.8k (player-made DoP; alt 4, port 0 only)"
echo 1 > /sys/module/snd_usb_mytek/parameters/dop352
play S32_LE 352800 s32_352k.raw
echo 0 > /sys/module/snd_usb_mytek/parameters/dop352
log "hw params offered"
aplay -D hw:0,0 --dump-hw-params -t raw -d 1 /dev/zero 2>&1 | grep -E 'FORMAT|RATE'
log "DSD64 as DoP at 176.4k, every DSD format"
play DSD_U8 352800 dsd64_u8.raw
play DSD_U16_LE 176400 dsd64_u16le.raw
play DSD_U16_BE 176400 dsd64_u16be.raw
play DSD_U32_LE 88200 dsd64_u32le.raw
play DSD_U32_BE 88200 dsd64_u32be.raw
log "DSD128 as DoP at 352.8k"
play DSD_U8 705600 dsd128_u8.raw
play DSD_U16_BE 352800 dsd128_u16be.raw
play DSD_U32_LE 176400 dsd128_u32le.raw
play DSD_U32_BE 176400 dsd128_u32be.raw
log "dsd=0: DSD formats must be refused"
echo 0 > /sys/module/snd_usb_mytek/parameters/dsd
aplay -q -D hw:0,0 -f DSD_U32_BE -r 88200 -c 2 -t raw -d 1 /dev/zero; echo "aplay dsd (param off) exit=$?"
echo 1 > /sys/module/snd_usb_mytek/parameters/dsd
log "PCM again after DSD"
play S32_LE 48000 s32_48k.raw
play S24_LE 176400 s24_176k.raw
log "unmute_delay=0"
echo 0 > /sys/module/snd_usb_mytek/parameters/unmute_delay
play DSD_U32_BE 88200 dsd64_u32be.raw
echo 700 > /sys/module/snd_usb_mytek/parameters/unmute_delay
log "mute_delay=150 dop_exit=100: ~17640 dop_exit_frames, bit-exact"
echo 150 > /sys/module/snd_usb_mytek/parameters/mute_delay
echo 100 > /sys/module/snd_usb_mytek/parameters/dop_exit
play DSD_U16_BE 352800 dsd128_u16be.raw
echo 20 > /sys/module/snd_usb_mytek/parameters/mute_delay
echo 200 > /sys/module/snd_usb_mytek/parameters/dop_exit
log "dop_preroll=1: DoP silence before the DoP data (preroll_zero_frames=0, dop=1), bit-exact"
echo 1 > /sys/module/snd_usb_mytek/parameters/dop_preroll
play DSD_U32_BE 88200 dsd64_u32be.raw
play DSD_U8 705600 dsd128_u8.raw
echo 0 > /sys/module/snd_usb_mytek/parameters/dop_preroll
log "legacy_ports=1: PCM and DSD64 with the 2.2.0 port setup (W02=0703 W04=0006), bit-exact"
echo 1 > /sys/module/snd_usb_mytek/parameters/legacy_ports
play S32_LE 48000 s32_48k.raw
play DSD_U32_BE 88200 dsd64_u32be.raw
log "legacy_ports=1 at 352.8k: must still use W02=0101"
play DSD_U16_LE 352800 dsd128_u16le.raw
echo 0 > /sys/module/snd_usb_mytek/parameters/legacy_ports
log "unmute=0 (diagnostic): no G00=01, vendor_seq=MISMATCH expected"
echo 0 > /sys/module/snd_usb_mytek/parameters/unmute
play S32_LE 48000 s32_48k.raw
echo 1 > /sys/module/snd_usb_mytek/parameters/unmute
log "DSD with zero-length feedback every 37 packets"
kill $EMU; sleep 1
/mytek-emu -z 37 & EMU=$!; wait_card
play DSD_U32_BE 88200 dsd64_u32be.raw
play DSD_U16_LE 352800 dsd128_u16le.raw
kill $EMU; sleep 1

log "zero-length feedback packets every 37 packets (zlp_fill=1)"
/mytek-emu -z 37 & EMU=$!; wait_card
play S32_LE 96000 s32_96k.raw
echo 2 > /sys/module/snd_usb_mytek/parameters/zlp_fill
log "same with zlp_fill=2 (driver 1.x)"
play S32_LE 96000 s32_96k.raw
echo 1 > /sys/module/snd_usb_mytek/parameters/zlp_fill
kill $EMU; sleep 1

log "feedback packet errors every 50 packets + out packet errors every 100 urbs"
/mytek-emu -e 50 -x 100 & EMU=$!; wait_card
play S32_LE 48000 s32_48k.raw
log "out_lead=0 (driver 1.x pacing)"
echo 0 > /sys/module/snd_usb_mytek/parameters/out_lead
play S32_LE 48000 s32_48k.raw
echo 6 > /sys/module/snd_usb_mytek/parameters/out_lead
kill $EMU; sleep 1

log "unplug during playback"
/mytek-emu -d 2 & EMU=$!; wait_card
aplay -q -D hw:0,0 -f S32_LE -r 192000 -c 2 -t raw /data/s32_192k.raw; echo "aplay exit=$? (error expected)"
sleep 2
cat /proc/asound/cards
log "replug after unplug"
/mytek-emu & EMU=$!; wait_card && cat /proc/asound/cards
play S32_LE 48000 s32_48k.raw
log "unplug while idle"
kill $EMU; sleep 2
log "rmmod"
rmmod snd_usb_mytek; echo "rmmod exit=$?"
sleep 1
log "kernel log check"
dmesg >> /tmp/klog
grep -E 'BUG:|KASAN|WARNING:|possible circular|possible recursive|INFO: task|Call Trace|UBSAN' /tmp/klog && echo "KERNEL-PROBLEMS-FOUND" || echo "KERNEL-LOG-CLEAN"
log "done"
poweroff -f
