#!/bin/sh
sysdir=/mnt/SDCARD/.tmp_update
export LD_LIBRARY_PATH="/lib:/config/lib:$sysdir/lib"
export PATH="$sysdir/bin:$PATH"
export SDL_VIDEODRIVER=mmiyoo
export SDL_AUDIODRIVER=mmiyoo
export EGL_VIDEODRIVER=mmiyoo

logfile=$(basename "$0" .sh)
. $sysdir/script/log.sh

MODEL_MM=283
MODEL_MMF=285
MODEL_MMP=354
screen_resolution="640x480"

KEY_UP=103
KEY_HELD_WAIT_MS=300
ONION_PARTITION=/dev/mmcblk0p2

main() {
    check_onion_boot

    # Set model ID based on hardware detection
    if [ -e /sys/devices/soc0/soc/soc:hall-mh248/hallvalue ] || [ -e /dev/input/event1 ]; then
        export DEVICE_ID=$MODEL_MMF
    elif axp 0 > /dev/null 2>&1; then
        export DEVICE_ID=$MODEL_MMP
    else
        export DEVICE_ID=$MODEL_MM
    fi
    echo -n "$DEVICE_ID" > /tmp/deviceModel

    touch /tmp/is_booting
    clear_logs

    init_system
    update_time

    # Remount passwd/group to add our own users
    mount -o bind $sysdir/config/passwd /etc/passwd
    mount -o bind $sysdir/config/group /etc/group

    # Start the battery monitor
    # batmon 2>&1 | tee -a "$sysdir/logs/Batmon.log" &
    batmon &

    # Check is charging
    if [ $DEVICE_ID -eq $MODEL_MM ]; then
        is_charging=$(cat /sys/devices/gpiochip0/gpio/gpio59/value)
    elif [ $DEVICE_ID -eq $MODEL_MMP ] || [ $DEVICE_ID -eq $MODEL_MMF ]; then
        axp_status="0x$(axp 0 | cut -d':' -f2)"
        is_charging=$([ $(($axp_status & 0x4)) -eq 4 ] && echo 1 || echo 0)
    fi

    sync

    # Show charging animation
    if [ $is_charging -eq 1 ]; then
        cd $sysdir
        chargingState
    fi

    cd $sysdir
    # bootScreen "Boot" 2>&1 | tee -a "$sysdir/logs/BootScreen.log"
    bootScreen "Boot"

    # Init
    rm /tmp/.offOrder 2> /dev/null
    HOME=/mnt/SDCARD/Stories/

    mkdir -m 777 -p /mnt/SDCARD/Saves

    rm -rf /tmp/is_booting

    flash_telmi_logo

    launch_storyteller

    while true; do
        check_off_order "End"
    done
}

# Dual boot: holding UP at power on starts OnionOS installed on the second
# SD card partition. It is mounted over /mnt/SDCARD so Onion runs as if it
# were alone on the card and never writes to the TelmiOS partition.
check_onion_boot() {
    keyHeld $KEY_UP $KEY_HELD_WAIT_MS || return
    onion_partition_ready || return

    log "\n:: Boot OnionOS from $ONION_PARTITION"
    sync
    if ! mount -t vfat "$ONION_PARTITION" /mnt/SDCARD; then
        log "Mount failed, boot TelmiOS"
        return
    fi
    if [ ! -f /mnt/SDCARD/.tmp_update/updater ]; then
        log "OnionOS not found, boot TelmiOS"
        umount /mnt/SDCARD
        return
    fi

    # Hand over to Onion with a clean environment, like the stock firmware does
    unset SDL_VIDEODRIVER SDL_AUDIODRIVER EGL_VIDEODRIVER
    export LD_LIBRARY_PATH="/lib:/config/lib"
    cd /mnt/SDCARD/.tmp_update
    exec ./updater
}

onion_partition_ready() {
    [ -b "$ONION_PARTITION" ] && return 0
    # /dev may lack partition nodes: create it from sysfs major:minor
    dev_numbers=$(cat /sys/class/block/$(basename "$ONION_PARTITION")/dev 2> /dev/null) || return 1
    mknod "$ONION_PARTITION" b "${dev_numbers%:*}" "${dev_numbers#*:}"
}

set_prev_state() {
    echo "$1" > /tmp/prev_state
}

clear_logs() {
    mkdir -p $sysdir/logs

    cd $sysdir/logs
    rm -f ./runtime.log 2> /dev/null
}

is_running() {
    process_name="$1"
    pgrep "$process_name" > /dev/null
}

get_info_value() {
    echo "$1" | grep "$2\b" | awk '{split($0,a,"="); print a[2]}' | awk -F'"' '{print $2}' | tr -d '\n'
}

flash_telmi_logo() {
    log "\n:: Flash Telmi Logo"

    # Beacon detection and logo name retrieving are done here, such that the actual flash script can also be called
    # during initial TelmiOS installation (as the beacon will be somewhere else, and there is no reboot extension)

    beaconfile="/mnt/SDCARD/Saves/.flashLogo" # "beacon" file (contains the logo name to be flashed, if file exists)
    rebootext=".reboot"  # optional "reboot" extension (indicates that a reboot is requested)

    log "Looking for a beacon at $beaconfile (with or without $rebootext extension)"

    # Check whether a "beacon" file exists (with or without reboot option), otherwise abort flashing
    if [ -f "$beaconfile" ]; then
        beacon="$beaconfile"
        reboot=0
    elif [ -f "$beaconfile$rebootext" ]; then
        beacon="$beaconfile$rebootext"
        reboot=1
    else  # Abort flashing if there is no "beacon" file
        log "No beacon detected. Abort."
        return
    fi

    logo=$(cat "$beacon")
    log "Beacon detected! Requested logo: $logo"
    rm -f "$beacon"  # The "beacon" file can now be safely deleted

    # Flashes the logo
    log "Running flashing script..."
    $sysdir/script/customlogo/flash_logo.sh "$logo" $reboot
    status=$?

    if [ $status -eq 0 ]; then  # flashing successful
        log "Flashing successful!"
    else  # flashing failed
        log "Flashing failed with status=$status"
    fi
}

launch_storyteller() {
    log "\n:: Launch Story Teller"
    cd $sysdir
    # LD_PRELOAD="$sysdir/lib/libpadsp.so" storyTeller 2>&1 | tee -a "$sysdir/logs/StoryTeller.log"
    LD_PRELOAD="$sysdir/lib/libpadsp.so" storyTeller
    sync
}

check_off_order() {
    if [ -f /tmp/.offOrder ]; then
        bootScreen "$1" &
        sleep 1 # Allow the bootScreen to be displayed
        shutdown
    fi
}

init_system() {
    log "\n:: Init system"

    load_settings

    # init_lcd
    cat /proc/ls
    sleep 0.25

    if [ $DEVICE_ID -eq $MODEL_MMP ] && [ -f $sysdir/config/.lcdvolt ]; then
        $sysdir/script/lcdvolt.sh 2> /dev/null
    fi

    brightness=$(/customer/app/jsonval brightness)
    brightness_raw=$(awk "BEGIN { print int(3 * exp(0.350656 * $brightness) + 0.5) }")
    log "brightness: $brightness -> $brightness_raw"

    # init backlight
    echo 0 > /sys/class/pwm/pwmchip0/export
    echo 800 > /sys/class/pwm/pwmchip0/pwm0/period
    echo $brightness_raw > /sys/class/pwm/pwmchip0/pwm0/duty_cycle
    echo 1 > /sys/class/pwm/pwmchip0/pwm0/enable

    get_screen_resolution
}

load_settings() {
    system_settings="/mnt/SDCARD/.tmp_update/res/miyoo${DEVICE_ID}_system.json"
    # echo $system_settings > "/mnt/SDCARD/.tmp_update/logs/runtime.log"
    if [ -f "$system_settings" ]; then
        cp -f "$system_settings" /mnt/SDCARD/system.json
    fi

    # link /appconfigs/system.json to SD card
    rm -f /appconfigs/system.json
    ln -s /mnt/SDCARD/system.json /appconfigs/system.json

    if [ $DEVICE_ID -eq $MODEL_MM ]; then
        # init charger detection
        if [ ! -f /sys/devices/gpiochip0/gpio/gpio59/direction ]; then
            echo 59 > /sys/class/gpio/export
            echo in > /sys/devices/gpiochip0/gpio/gpio59/direction
        fi

        if [ $(/customer/app/jsonval vol) -ne 20 ] || [ $(/customer/app/jsonval mute) -ne 0 ]; then
            # Force volume and mute settings
            cat /mnt/SDCARD/system.json |
                sed 's/^\s*"vol":\s*[0-9][0-9]*/\t"vol":\t20/g' |
                sed 's/^\s*"mute":\s*[0-9][0-9]*/\t"mute":\t0/g' \
                    > temp
            mv -f temp /mnt/SDCARD/system.json
        fi
    fi
}

get_screen_resolution() {
    max_attempts=10
    attempt=0

    while [ "$attempt" -lt "$max_attempts" ]; do
        screen_resolution=$(grep 'Current TimingWidth=' /proc/mi_modules/fb/mi_fb0 | sed 's/Current TimingWidth=\([0-9]*\),TimingWidth=\([0-9]*\),.*/\1x\2/')
        if [ -n "$screen_resolution" ]; then
            break
        fi
        attempt=$((attempt + 1))
        sleep 0.5
    done

    if [ "$screen_resolution" = "752x560" ] && [ "$(/etc/fw_printenv miyoo_version | cut -d'=' -f2)" -ge "202310271401" ]; then
        touch /tmp/new_res_available
    else
        screen_resolution="640x480"
    fi

    echo -n "$screen_resolution" > /tmp/screen_resolution
}

update_time() {
    timepath=/mnt/SDCARD/Saves/CurrentProfile/saves/currentTime.txt
    currentTime=0
    # Load current time
    if [ -f $timepath ]; then
        currentTime=$(cat $timepath)
    fi
    date +%s -s @$currentTime

    # Ensure that all play activities are closed
    playActivity stop_all

    #Add 4 hours to the current time
    hours=4
    if [ -f $sysdir/config/startup/addHours ]; then
        hours=$(cat $sysdir/config/startup/addHours)
    fi
    addTime=$(($hours * 3600))
    if [ ! -f $sysdir/config/.ntpState ]; then
        currentTime=$(($currentTime + $addTime))
    fi
    date +%s -s @$currentTime
}

kill_audio_servers() {
    $sysdir/script/stop_audioserver.sh
}

runifnecessary() {
    cnt=0
    #a=`ps | grep $1 | grep -v grep`
    a=$(pgrep $1)
    while [ "$a" == "" ] && [ $cnt -lt 8 ]; do
        log "try to run: $2"
        $2 $3 &
        sleep 0.5
        cnt=$(expr $cnt + 1)
        a=$(pgrep $1)
    done
}

check_timezone() {
    export TZ=$(cat "$sysdir/config/.tz")
}

if [ -f $sysdir/config/.logging ]; then
    main
else
    main 2>&1 > /dev/null
fi
