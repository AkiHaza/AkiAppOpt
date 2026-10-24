MODDIR=${0%/*}
PID_FILE="$MODDIR/AppOpt.pid"
LOG_FILE="$MODDIR/AppOpt.log"
MAX_LOG_BYTES=65536

log_start_event() {
	printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" >> "$LOG_FILE"
}

is_appopt_process() {
	[ -n "$1" ] && [ -r "/proc/$1/cmdline" ] || return 1
	PROCESS_CMD=$(tr '\0' ' ' < "/proc/$1/cmdline" 2>/dev/null)
	case "$PROCESS_CMD" in
		"$MODDIR/AppOpt"*) return 0 ;;
	esac
	return 1
}

stop_late_load_instance() {
	if command -v pidof >/dev/null 2>&1; then
		APP_OPT_PIDS=$(pidof AppOpt 2>/dev/null)
	elif [ -f "$PID_FILE" ]; then
		APP_OPT_PIDS=$(cat "$PID_FILE" 2>/dev/null)
	fi

	for APP_OPT_PID in $APP_OPT_PIDS; do
		is_appopt_process "$APP_OPT_PID" || continue
		kill "$APP_OPT_PID" 2>/dev/null || continue
		i=5
		while kill -0 "$APP_OPT_PID" 2>/dev/null && [ "$i" -gt 0 ]; do
			i=$((i-1))
			sleep 1
		done
		kill -9 "$APP_OPT_PID" 2>/dev/null
	done
	rm -f "$PID_FILE"
}

wait_sys_boot_completed() {
	local i=9
	until [ "$(getprop sys.boot_completed)" == "1" ] || [ $i -le 0 ]; do
		i=$((i-1))
		sleep 9
	done
}

# KernelSU late-load 软启动后重建 AppOpt，正常启动模式不进入此分支。
if [ "$KSU_LATE_LOAD" = "1" ]; then
	stop_late_load_instance
fi

wait_sys_boot_completed

if [ ! -x "$MODDIR/AppOpt" ]; then
	: > "$LOG_FILE"
	log_start_event "AppOpt binary is missing or not executable"
	rm -f "$PID_FILE"
	exit 1
fi

if [ -f "$PID_FILE" ]; then
	OLD_PID=$(cat "$PID_FILE" 2>/dev/null)
	if is_appopt_process "$OLD_PID"; then
		APP_OPT_RUNNING=1
	fi
fi

if [ "$APP_OPT_RUNNING" != "1" ]; then
	: > "$LOG_FILE"
	log_start_event "starting AppOpt"
	LOG_PIPE="$MODDIR/.AppOpt.log.$$"
	rm -f "$LOG_PIPE"
	if ! mkfifo "$LOG_PIPE"; then
		log_start_event "cannot create startup log pipe"
		rm -f "$PID_FILE"
		exit 1
	fi
	# Keep the first 64 KiB for diagnosis, then drain output without growing the log.
	( trap '' HUP; head -c "$MAX_LOG_BYTES" >> "$LOG_FILE"; cat > /dev/null ) < "$LOG_PIPE" &
	nohup "$MODDIR/AppOpt" -c "$MODDIR/applist.conf" > "$LOG_PIPE" 2>&1 &
	NEW_PID=$!
	echo "$NEW_PID" > "$PID_FILE"
	sleep 1
	rm -f "$LOG_PIPE"
	if ! kill -0 "$NEW_PID" 2>/dev/null; then
		log_start_event "AppOpt exited during startup"
		rm -f "$PID_FILE"
		exit 1
	fi
fi

for MAX_CPUS in /sys/devices/system/cpu/cpu*/core_ctl/max_cpus; do
	[ -e "$MAX_CPUS" ] || continue
	MIN_CPUS="${MAX_CPUS%/*}/min_cpus"
	if [ -e "$MIN_CPUS" ] && [ "$(cat "$MAX_CPUS")" != "$(cat "$MIN_CPUS")" ]; then
		chmod a+w "${MAX_CPUS%/*}/min_cpus"
		cat "$MAX_CPUS" > "$MIN_CPUS"
		chmod a-w "${MAX_CPUS%/*}/min_cpus"
	fi
done

# 如需暂停绿厂 oiface，请取消下一行注释；恢复时将 0 改为 1。
# [ -n "$(getprop persist.sys.oiface.enable)" ] && setprop persist.sys.oiface.enable 0

# 如需禁用米系机型 joyose，请取消下一行注释。
# pm disable-user com.xiaomi.joyose; pm clear com.xiaomi.joyose
