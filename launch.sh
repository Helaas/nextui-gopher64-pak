#!/bin/sh
PAK_DIR="$(dirname "$0")"
PAK_NAME="$(basename "$PAK_DIR")"
PAK_NAME="${PAK_NAME%.*}"
set -x

rm -f "$LOGS_PATH/$PAK_NAME.txt"
exec >>"$LOGS_PATH/$PAK_NAME.txt"
exec 2>&1

echo "$0" "$@"
cd "$PAK_DIR" || exit 1
mkdir -p "$USERDATA_PATH/$PAK_NAME"

architecture=arm
if uname -m | grep -q '64'; then
	architecture=arm64
fi

export HOME="$USERDATA_PATH/$PAK_NAME"
export LD_LIBRARY_PATH="$PAK_DIR/lib/$PLATFORM:$LD_LIBRARY_PATH"
export PATH="$PAK_DIR/bin/$architecture:$PAK_DIR/bin/$PLATFORM:$PAK_DIR/bin:$PATH"

export ROM_NAME="$(basename -- "$*")"
export GAMESETTINGS_DIR="$USERDATA_PATH/$PAK_NAME/game-settings/$ROM_NAME"

get_cpu_mode() {
	cpu_mode="ondemand"
	if [ -f "$GAMESETTINGS_DIR/cpu-mode" ]; then
		cpu_mode="$(cat "$GAMESETTINGS_DIR/cpu-mode")"
	fi
	if [ -f "$GAMESETTINGS_DIR/cpu-mode.tmp" ]; then
		cpu_mode="$(cat "$GAMESETTINGS_DIR/cpu-mode.tmp")"
	fi
	echo "$cpu_mode"
}

write_settings_json() {
	cpu_mode="$(get_cpu_mode)"

	jq -rM '{settings: .settings}' "$PAK_DIR/settings.json" >"$GAMESETTINGS_DIR/settings.json"

	update_setting_key "$GAMESETTINGS_DIR/settings.json" "CPU Mode" "$cpu_mode"
	sync
}

update_setting_key() {
	settings_file="$1"
	setting_name="$2"
	option_value="$3"

	jq --arg name "$setting_name" --arg option "$option_value" '
		.settings |= map(if .name == $name then . + {"selected": ((.options // []) | index($option) // -1)} else . end)
	' "$settings_file" >"$settings_file.tmp"
	mv -f "$settings_file.tmp" "$settings_file"
}

settings_menu() {
	mkdir -p "$GAMESETTINGS_DIR"

	rm -f "$GAMESETTINGS_DIR/cpu-mode.tmp"

	write_settings_json

	r2_value="$(coreutils timeout .1s evtest /dev/input/event3 2>/dev/null | awk '/ABS_RZ/{getline; print}' | awk '{print $2}' || true)"
	if [ "$r2_value" = "255" ]; then
		while true; do
			minui_list_output="$(minui-list --disable-auto-sleep --file "$GAMESETTINGS_DIR/settings.json" --item-key "settings" --title "N64 Gopher64 Settings" --action-button "X" --action-text "PLAY" --write-value state --confirm-text "CONFIRM")" || {
				exit_code="$?"
				if [ "$exit_code" -eq 4 ]; then
					# shellcheck disable=SC2016
					echo "$minui_list_output" | jq -r --arg name "CPU Mode" '.settings[] | select(.name == $name) | .options[.selected]' >"$GAMESETTINGS_DIR/cpu-mode.tmp"
					break
				fi
				if [ "$exit_code" -ne 0 ]; then
					exit "$exit_code"
				fi
			}

			cpu_mode="$(echo "$minui_list_output" | jq -r --arg name "CPU Mode" '.settings[] | select(.name == $name) | .options[.selected]')"

			echo "$minui_list_output" >"$GAMESETTINGS_DIR/settings.json"
			echo "$cpu_mode" >"$GAMESETTINGS_DIR/cpu-mode"
			sync
		done
	fi
}

get_rom_path() {
	if [ -z "$TEMP_ROM" ]; then
		return
	fi

	ROM_PATH=""
	case "$*" in
	*.n64 | *.v64 | *.z64)
		ROM_PATH="$*"
		;;
	*.zip | *.7z)
		existing_governor="$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"
		existing_max_freq="$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq)"
		echo performance >/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
		echo 2160000 >/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq
		ROM_PATH="$TEMP_ROM"

		7zzs e "$*" -so >"$TEMP_ROM"
		echo "$existing_governor" >/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
		echo "$existing_max_freq" >/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq
		;;
	esac

	echo "$ROM_PATH"
}

configure_cpu() {
	cpu_mode="$(get_cpu_mode)"

	cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor >"$HOME/cpu_governor.txt"
	cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq >"$HOME/cpu_min_freq.txt"
	cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq >"$HOME/cpu_max_freq.txt"

	if [ "$cpu_mode" = "performance" ]; then
		echo performance >/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
		echo 1200000 >/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq
		echo 2160000 >/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq
	else
		echo ondemand >/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
		echo 408000 >/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq
		echo 2160000 >/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq
	fi
}

show_message() {
	message="$1"
	seconds="$2"

	if [ -z "$seconds" ]; then
		seconds="forever"
	fi

	killall minui-presenter >/dev/null 2>&1 || true
	echo "$message" 1>&2
	if [ "$seconds" = "forever" ]; then
		minui-presenter --message "$message" --timeout -1 &
	else
		minui-presenter --message "$message" --timeout "$seconds"
	fi
}

sync_shared_saves_into_portable() {
	if [ -z "$SHARED_SAVE_DIR" ] || [ -z "$GOPHER_SAVE_DIR" ]; then
		return
	fi

	mkdir -p "$SHARED_SAVE_DIR" "$GOPHER_SAVE_DIR"
	cp -f "$SHARED_SAVE_DIR"/* "$GOPHER_SAVE_DIR"/ 2>/dev/null || true
}

sync_portable_saves_into_shared() {
	if [ -z "$SHARED_SAVE_DIR" ] || [ -z "$GOPHER_SAVE_DIR" ]; then
		return
	fi

	mkdir -p "$SHARED_SAVE_DIR" "$GOPHER_SAVE_DIR"
	cp -f "$GOPHER_SAVE_DIR"/* "$SHARED_SAVE_DIR"/ 2>/dev/null || true
}

cleanup() {
	rm -f "/tmp/stay_awake"
	rm -f "/tmp/gopher64.pid"
	rm -f "/tmp/force-power-off" "/tmp/force-power-off-tracker" "/tmp/force-exit"
	killall minui-presenter >/dev/null 2>&1 || true

	sync_portable_saves_into_shared

	if [ -f "$HOME/cpu_governor.txt" ] && [ -f "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor" ]; then
		governor="$(cat "$HOME/cpu_governor.txt")"
		echo "$governor" >/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
		rm -f "$HOME/cpu_governor.txt"
	fi
	if [ -f "$HOME/cpu_min_freq.txt" ] && [ -f "/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq" ]; then
		min_freq="$(cat "$HOME/cpu_min_freq.txt")"
		echo "$min_freq" >/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq
		rm -f "$HOME/cpu_min_freq.txt"
	fi
	if [ -f "$HOME/cpu_max_freq.txt" ] && [ -f "/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq" ]; then
		max_freq="$(cat "$HOME/cpu_max_freq.txt")"
		echo "$max_freq" >/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq
		rm -f "$HOME/cpu_max_freq.txt"
	fi

	if [ -f "$TEMP_ROM" ]; then
		rm -f "$TEMP_ROM"
	fi

	sync
}

main() {
	echo "1" >/tmp/stay_awake
	trap "cleanup" EXIT INT TERM HUP QUIT

	if [ "$PLATFORM" != "tg5050" ]; then
		show_message "$PLATFORM is not a supported platform" 2
		exit 1
	fi

	if [ ! -f "$1" ]; then
		show_message "ROM not found" 2
		exit 1
	fi

	TEMP_ROM=$(mktemp)
	ROM_PATH="$(TEMP_ROM="$TEMP_ROM" get_rom_path "$*")"
	if [ -z "$ROM_PATH" ]; then
		return
	fi

	mkdir -p "$HOME"

	settings_menu
	configure_cpu

	# Set up gopher64 in portable mode
	GOPHER64_DIR="$PAK_DIR/bin/$PLATFORM"
	touch "$GOPHER64_DIR/portable.txt"
	mkdir -p "$GOPHER64_DIR/portable_data/config"
	mkdir -p "$GOPHER64_DIR/portable_data/data/saves"
	mkdir -p "$GOPHER64_DIR/portable_data/data/states"

	# Write gopher64 config for fullscreen + gamepad
	cat >"$GOPHER64_DIR/portable_data/config/config.json" <<-CONFIGEOF
	{
	  "input": {
	    "input_profiles": {
	      "default": $(cat "$PAK_DIR/data/default_input_profile.json")
	    },
	    "input_profile_binding": ["default", "default", "default", "default"],
	    "controller_assignment": [null, null, null, null],
	    "controller_enabled": [true, false, false, false],
	    "transfer_pak": [false, false, false, false],
	    "emulate_vru": false
	  },
	  "video": {
	    "upscale": 1,
	    "integer_scaling": false,
	    "fullscreen": true,
	    "widescreen": false,
	    "crt": false
	  },
	  "emulation": {
	    "overclock": false,
	    "disable_expansion_pak": false,
	    "usb": false
	  },
	  "rom_dir": ""
	}
	CONFIGEOF

	# Filesystems like exFAT cannot create symlinks in pak folders,
	# so mirror saves between shared userdata and portable_data instead.
	SHARED_SAVE_DIR="$SHARED_USERDATA_PATH/$PAK_NAME"
	GOPHER_SAVE_DIR="$GOPHER64_DIR/portable_data/data/saves"
	sync_shared_saves_into_portable

	show_message "Loading..." forever
	sleep 0.1
	killall minui-presenter >/dev/null 2>&1 || true

	gopher64 --fullscreen "$ROM_PATH" &
	PROCESS_PID="$!"
	echo "$PROCESS_PID" >"/tmp/gopher64.pid"

	while kill -0 "$PROCESS_PID" 2>/dev/null; do
		sleep 1
	done

	if [ -f "/tmp/force-power-off" ]; then
		sync
		rm /tmp/minui_exec
		poweroff
		while :; do
			sleep 1
		done
	fi
}

main "$@"
