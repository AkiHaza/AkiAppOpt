SKIPUNZIP=0
check_magisk_version() {
	ui_print "- Magisk version: $MAGISK_VER_CODE"
	ui_print "- Module version: $(grep_prop version "${TMPDIR}/module.prop")"
	ui_print "- Module versionCode: $(grep_prop versionCode "${TMPDIR}/module.prop")"
	ui_print "********************************************"
	ui_print "- $(grep_prop description "${TMPDIR}/module.prop")"
	if [ "$MAGISK_VER_CODE" -lt 20400 ]; then
		ui_print "********************************************"
		ui_print "! 请安装 Magisk v20.4+ (20400+)"
		abort    "********************************************"
	fi
}
check_required_files() {
	REQUIRED_FILE_LIST="/sys/devices/system/cpu/present /proc/loadavg"
	for REQUIRED_FILE in $REQUIRED_FILE_LIST; do
		if [ ! -e "$REQUIRED_FILE" ]; then
			ui_print "********************************************"
			ui_print "! $REQUIRED_FILE 文件不存在"
			ui_print "! 请联系模块作者"
			abort    "********************************************"
		fi
	done
}
extract_bin() {
	ui_print "********************************************"
	if [ "$ARCH" == "arm" ]; then
		cp "$MODPATH/bin/armabi-v7a/AppOpt" "$MODPATH/AppOpt"
	elif [ "$ARCH" == "arm64" ]; then
		cp "$MODPATH/bin/arm64-v8a/AppOpt" "$MODPATH/AppOpt"
	elif [ "$ARCH" == "x86" ]; then
		cp "$MODPATH/bin/x86/AppOpt" "$MODPATH/AppOpt"
	elif [ "$ARCH" == "x64" ]; then
		cp "$MODPATH/bin/x86_64/AppOpt" "$MODPATH/AppOpt"
	else
		abort "! Unsupported platform: $ARCH"
	fi
	ui_print "- Device platform: $ARCH"
	if [ ! -f "$MODPATH/AppOpt" ]; then
		abort "! 当前架构缺少 AppOpt 二进制文件"
	fi
	rm -rf "$MODPATH/bin"
	[ -f "$MODPATH/AppOpt" ] && chmod a+x "$MODPATH/AppOpt"
	if ! "$MODPATH/AppOpt" -v; then
		abort "! 主程序验证失败，请检查模块zip文件是否损坏"
	fi
}
remove_sys_perf_config() {
	for SYSPERFCONFIG in /system/vendor/bin/msm_irqbalance; do
		[ -e "$SYSPERFCONFIG" ] || continue
		[ -d "$MODPATH${SYSPERFCONFIG%/*}" ] || mkdir -p "$MODPATH${SYSPERFCONFIG%/*}"
		ui_print "- Remove :$SYSPERFCONFIG"
		touch "$MODPATH$SYSPERFCONFIG"
	done
	if [ -n "$(pm path com.xiaomi.joyose)" ] && [ -n "$(getprop ro.miui.ui.version.code)" ]; then
		pm disable --user 0 com.xiaomi.joyose/.smartop.SmartOpService
		echo 'pm enable com.xiaomi.joyose/.smartop.SmartOpService' >> "$MODPATH/uninstall.sh"
	fi
}
module_instructions() {
	ui_print "********************************************"
	ui_print "线程规则配置文件路径为："
	ui_print "/data/adb/modules/AkiAppOpt/applist.conf"
	ui_print "------------------------------------------"
	ui_print "修改与添加规则无需重启，即时生效"
	ui_print "AppOpt 启动时自动识别 CPU 频率簇"
	ui_print "规则可直接使用 e-core / p-core / hp-core / all-core"
	ui_print "语义核心可与数字范围使用英文逗号组合"
	ui_print "------------------------------------------"
	ui_print "规则示例："
	ui_print "com.example=all-core"
	ui_print "com.example{RenderThread}=hp-core"
	ui_print "------------------------------------------"
	ui_print "更多规则使用说明请参考："
	ui_print "http://AppOpt.suto.top"
	ui_print "********************************************"
}
preserve_existing_config() {
	OLD_CONFIG=/data/adb/modules/AkiAppOpt/applist.conf
	OLD_DEFAULTS=/data/adb/modules/AkiAppOpt/applist.defaults.conf
	NEW_DEFAULTS="$MODPATH/applist.defaults.conf"
	MERGE_TEMP="$MODPATH/.applist.merge.$$"
	# Capture the shipped defaults before replacing the live, editable file.
	cp "$MODPATH/applist.conf" "$NEW_DEFAULTS" || abort "! 无法保存默认规则基线"
	[ -f "$OLD_CONFIG" ] || return 0
	cp "$OLD_CONFIG" "$MODPATH/applist.conf.previous" || abort "! 无法备份现有配置"
	if [ -f "$OLD_DEFAULTS" ]; then
		if LC_ALL=C awk -f "$MODPATH/config_merge.awk" \
			"$OLD_DEFAULTS" "$OLD_CONFIG" "$NEW_DEFAULTS" > "$MERGE_TEMP"; then
			ui_print "- 已更新默认规则并保留用户新增、修改和删除"
		else
			# Keep the old config and its baseline together so a later retry is safe.
			cp "$OLD_CONFIG" "$MERGE_TEMP" || abort "! 无法保留现有配置"
			cp "$OLD_DEFAULTS" "$NEW_DEFAULTS" || abort "! 无法保留原规则基线"
			ui_print "! 配置合并未完成，已保留原配置与基线，未覆盖用户规则"
		fi
	else
		# Pre-feature installations have no trustworthy defaults to compare against.
		cp "$OLD_CONFIG" "$MERGE_TEMP" || abort "! 无法迁移现有配置"
		ui_print "- 首次迁移：完整保留现有配置，已记录默认规则基线"
	fi
	mv -f "$MERGE_TEMP" "$MODPATH/applist.conf" || abort "! 无法保存升级配置"
}
check_magisk_version
check_required_files
extract_bin
remove_sys_perf_config
module_instructions
preserve_existing_config
set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/AppOpt" 0 2000 0755 u:object_r:magisk_file:s0
for SCRIPT in "$MODPATH"/*.sh; do
	[ -f "$SCRIPT" ] || continue
	set_perm "$SCRIPT" 0 2000 0755 u:object_r:magisk_file:s0
done
