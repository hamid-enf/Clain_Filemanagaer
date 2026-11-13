#!/bin/sh
# check_board.sh -- two settings of the CubeMX project the library depends
# on.  Both live in files CubeMX writes, both were wrong once, and a host
# build never sees either:
#
#   1. FATFS/Target/ffconf.h: _FS_LOCK (FF_FS_LOCK) is the number of files
#      and folders FatFS keeps open at once.  The library opens up to
#      FM_MAX_OPEN_FILES + FM_MAX_OPEN_DIRS; with the CubeMX value of 2 a
#      recursive copy failed three folders deep with FM_ERR_BUSY.  (The
#      adapter refuses to compile with a smaller table, so a firmware
#      build would stop too -- this says it before one is started.)
#
#   2. Interrupt priorities: the USB mass-storage callbacks run inside
#      OTG_FS_IRQHandler and wait for the SD card / the flash with HAL
#      timeouts, which count HAL_GetTick().  If the 1 ms tick cannot
#      interrupt that handler, a stalled card is a frozen MCU.  The tick
#      priority (HAL_InitTick(n) in main.c, else TICK_INT_PRIORITY) must
#      be a smaller number than the last one given to OTG_FS_IRQn.
#
# The project is the folder above the library; without one (the library
# copied somewhere else) there is nothing to check.
#
#     sh tools/check_board.sh            [BOARD=<project folder>]
cd "$(dirname "$0")/.." || exit 1

BOARD=${BOARD:-..}
FFCONF=$BOARD/FATFS/Target/ffconf.h
MAINC=$BOARD/Core/Src/main.c
HALCONF=$BOARD/Core/Inc/stm32h7xx_hal_conf.h
USBCONF=$BOARD/USB_DEVICE/Target/usbd_conf.c
rc=0
fail() { echo "FAIL board: $1"; rc=1; }

# the value of "#define NAME value" in a file ($3: text when it is absent)
define_of() {
    v=$(tr -d '\r' < "$1" | sed -n -E \
        "s/^[[:space:]]*#[[:space:]]*define[[:space:]]+$2[[:space:]]+\(?([0-9]+).*/\1/p" | tail -1)
    echo "${v:-$3}"
}

if [ ! -f "$FFCONF" ] && [ ! -f "$MAINC" ]; then
    echo "board: no CubeMX project at $BOARD, nothing to check"
    exit 0
fi

# ---- 1. the table of open objects ---------------------------------------
if [ -f "$FFCONF" ]; then
    files=$(define_of include/fm_config.h FM_MAX_OPEN_FILES "")
    dirs=$(define_of include/fm_config.h FM_MAX_OPEN_DIRS "")
    lock=$(define_of "$FFCONF" _FS_LOCK "")
    [ -n "$lock" ] || lock=$(define_of "$FFCONF" FF_FS_LOCK "")
    if [ -z "$files" ] || [ -z "$dirs" ]; then
        fail "FM_MAX_OPEN_FILES / FM_MAX_OPEN_DIRS not found in include/fm_config.h (update this script)"
    elif [ -z "$lock" ]; then
        fail "$FFCONF defines neither _FS_LOCK nor FF_FS_LOCK"
    elif [ "$lock" -ne 0 ] && [ "$lock" -lt $((files + dirs)) ]; then
        fail "$FFCONF: _FS_LOCK is $lock, the library opens up to $((files + dirs)) files and folders (set it to 16; CubeMX: FATFS -> FS_LOCK)"
    else
        echo "board: _FS_LOCK $lock holds the $((files + dirs)) files and folders the library opens"
    fi
fi

# ---- 2. the tick can interrupt the USB interrupt ------------------------
if [ -f "$USBCONF" ] && [ -f "$MAINC" ]; then
    usb=$(tr -d '\r' < "$USBCONF" | grep -v '^[[:space:]]*//' | sed -n -E \
        's/.*HAL_NVIC_SetPriority\([[:space:]]*OTG_FS_IRQn[[:space:]]*,[[:space:]]*([0-9]+).*/\1/p' | tail -1)
    tick=$(tr -d '\r' < "$MAINC" | grep -v '^[[:space:]]*//' | sed -n -E \
        's/.*HAL_InitTick\([[:space:]]*([0-9]+)[uUlL]*[[:space:]]*\).*/\1/p' | tail -1)
    from="HAL_InitTick() in main.c"
    if [ -z "$tick" ] && [ -f "$HALCONF" ]; then
        tick=$(define_of "$HALCONF" TICK_INT_PRIORITY "")
        from="TICK_INT_PRIORITY"
    fi
    if [ -z "$usb" ]; then
        echo "board: OTG_FS_IRQn gets no priority in $USBCONF, nothing to check"
    elif [ -z "$tick" ]; then
        fail "the priority of the 1 ms tick was not found (HAL_InitTick(n) in main.c or TICK_INT_PRIORITY)"
    elif [ "$tick" -ge "$usb" ]; then
        fail "the 1 ms tick has priority $tick ($from), OTG_FS_IRQn has $usb: HAL timeouts inside the USB interrupt never end (tick must be the smaller number)"
    else
        echo "board: the 1 ms tick (priority $tick) can interrupt the USB interrupt (priority $usb)"
    fi
fi
exit $rc
