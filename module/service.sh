#!/system/bin/sh
MODDIR=${0%/*}
PID_FILE="$MODDIR/AppOpt.pid"
LOG_FILE="$MODDIR/AppOpt.log"
MAX_LOG_BYTES=65536
BOOT_ID=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null)

log_start_event() {
	printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" >> "$LOG_FILE"
}

# comm can contain spaces and parentheses; strip through its last closing ).
read_process_identity() {
	case "$1" in ''|*[!0-9]*|0) return 1 ;; esac
	IFS= read -r PROCESS_STAT < "/proc/$1/stat" 2>/dev/null || return 1
	PROCESS_FIELDS=${PROCESS_STAT##*) }
	[ "$PROCESS_FIELDS" != "$PROCESS_STAT" ] || return 1
	set -- $PROCESS_FIELDS
	[ "$#" -ge 20 ] || return 1
	case "$1" in Z|X|x) return 1 ;; esac
	shift 19
	PROCESS_START=$1
	case "$PROCESS_START" in ''|*[!0-9]*) return 1 ;; esac
}

identity_is_alive() {
	[ -n "$BOOT_ID" ] && [ "$3" = "$BOOT_ID" ] || return 1
	read_process_identity "$1" && [ "$PROCESS_START" = "$2" ]
}

is_appopt_process() {
	read_process_identity "$1" || return 1
	PROCESS_CMD=$(tr '\0' '\n' < "/proc/$1/cmdline" 2>/dev/null | head -n 1)
	[ "$PROCESS_CMD" = "$MODDIR/AppOpt" ]
}

appopt_identity_is_alive() {
	identity_is_alive "$1" "$2" "$3" &&
		is_appopt_process "$1" && [ "$PROCESS_START" = "$2" ]
}

read_pid_record() {
	[ -r "$PID_FILE" ] || return 1
	IFS=' ' read -r RECORD_PID RECORD_START RECORD_BOOT RECORD_EXTRA < "$PID_FILE" 2>/dev/null || return 1
	[ -z "$RECORD_EXTRA" ] &&
		appopt_identity_is_alive "$RECORD_PID" "$RECORD_START" "$RECORD_BOOT"
}

write_pid_record() {
	RECORD_TMP="$PID_FILE.$$"
	(umask 077; printf '%s %s %s\n' "$1" "$2" "$BOOT_ID" > "$RECORD_TMP") &&
		mv -f "$RECORD_TMP" "$PID_FILE"
}

stop_appopt_identity() {
	appopt_identity_is_alive "$1" "$2" "$3" || return 0
	kill "$1" 2>/dev/null || return 1
	local remaining=5
	while appopt_identity_is_alive "$1" "$2" "$3" && [ "$remaining" -gt 0 ]; do
		remaining=$((remaining-1))
		sleep 1 9>&-
	done
	if appopt_identity_is_alive "$1" "$2" "$3"; then
		kill -9 "$1" 2>/dev/null
	fi
}

stop_late_load_instance() {
	if read_pid_record; then
		stop_appopt_identity "$RECORD_PID" "$RECORD_START" "$RECORD_BOOT"
	fi
	# Legacy records are accepted only after capturing an exact binary identity.
	if command -v pidof >/dev/null 2>&1; then
		APP_OPT_PIDS=$(pidof AppOpt 2>/dev/null)
	elif [ -f "$PID_FILE" ]; then
		IFS=' ' read -r APP_OPT_PIDS _ < "$PID_FILE"
	fi
	for APP_OPT_PID in $APP_OPT_PIDS; do
		if is_appopt_process "$APP_OPT_PID"; then
			stop_appopt_identity "$APP_OPT_PID" "$PROCESS_START" "$BOOT_ID"
		fi
	done
	rm -f "$PID_FILE"
}

wait_sys_boot_completed() {
	local remaining=9
	until [ "$(getprop sys.boot_completed)" = "1" ] || [ "$remaining" -le 0 ]; do
		remaining=$((remaining-1))
		sleep 9 9>&-
	done
}

wait_sys_boot_completed
[ -n "$BOOT_ID" ] || exit 1
[ ! -f "$MODDIR/disable" ] && [ ! -f "$MODDIR/remove" ] || exit 0

if [ ! -x "$MODDIR/AppOpt" ]; then
	log_start_event "AppOpt binary is missing or not executable"
	exit 1
fi

# The helper locks this shared description; no shell flock applet is required.
# Never unlink these lock files while an instance may still hold the inode.
exec 9>> "$MODDIR/.AppOpt.service.lock" || exit 1
"$MODDIR/AppOpt" --acquire-service-lock
LOCK_STATUS=$?
[ "$LOCK_STATUS" -eq 73 ] && exit 0
[ "$LOCK_STATUS" -eq 0 ] || exit 1

if [ "$KSU_LATE_LOAD" = "1" ]; then
	stop_late_load_instance
fi

if ! read_pid_record; then
	: > "$LOG_FILE"
	log_start_event "starting AppOpt"
	LOG_PIPE="$MODDIR/.AppOpt.log.$$"
	rm -f "$LOG_PIPE"
	if ! mkfifo "$LOG_PIPE"; then
		log_start_event "cannot create startup log pipe"
		exit 1
	fi
	( trap '' HUP; head -c "$MAX_LOG_BYTES" >> "$LOG_FILE"; cat > /dev/null ) 9>&- < "$LOG_PIPE" &
	nohup "$MODDIR/AppOpt" -c "$MODDIR/applist.conf" 9>&- > "$LOG_PIPE" 2>&1 &
	NEW_PID=$!
	sleep 1 9>&-
	rm -f "$LOG_PIPE"
	if ! is_appopt_process "$NEW_PID"; then
		wait "$NEW_PID"
		START_STATUS=$?
		[ "$START_STATUS" -eq 73 ] && exit 0
		log_start_event "AppOpt exited during startup (status $START_STATUS)"
		rm -f "$PID_FILE"
		exit 1
	fi
	write_pid_record "$NEW_PID" "$PROCESS_START" || {
		stop_appopt_identity "$NEW_PID" "$PROCESS_START" "$BOOT_ID"
		exit 1
	}
fi

for MAX_CPUS in /sys/devices/system/cpu/cpu*/core_ctl/max_cpus; do
	[ -e "$MAX_CPUS" ] || continue
	MIN_CPUS="${MAX_CPUS%/*}/min_cpus"
	if [ -e "$MIN_CPUS" ] && [ "$(cat "$MAX_CPUS")" != "$(cat "$MIN_CPUS")" ]; then
		chmod a+w "$MIN_CPUS"
		cat "$MAX_CPUS" > "$MIN_CPUS"
		chmod a-w "$MIN_CPUS"
	fi
done

# 如需暂停绿厂 oiface，请取消下一行注释；恢复时将 0 改为 1。
# [ -n "$(getprop persist.sys.oiface.enable)" ] && setprop persist.sys.oiface.enable 0

# 如需禁用米系机型 joyose，请取消下一行注释。
# pm disable-user com.xiaomi.joyose; pm clear com.xiaomi.joyose
