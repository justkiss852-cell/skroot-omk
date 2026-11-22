#!/system/bin/sh
# ---------------------------------------------------------------------------
# Oh My Keymint on SKRoot —— 启动脚本
# 等价于 OMK 官方模块的 post-fs-data.sh + service.sh：
#   1) 准备 /data/misc/keystore/omk（keystore 侧配置目录，属主 1017）
#   2) 准备 /data/adb/omk（状态目录：pid、payload、restart 标记）
#   3) 拉起 keymint 与 injector 两个常驻守护进程
# 由 SKRoot 模块入口（module_oh_my_keymint.cpp）在开机早期 fork 后调用。
# ---------------------------------------------------------------------------
MODDIR=${0%/*}
MODDIR=${MODDIR%/}
STATE_DIR=/data/adb/omk
DATA_DIR=/data/misc/keystore/omk
LOG_DIR=$DATA_DIR/logs

echo "=== skroot_start.sh $(date) ==="
echo "MODDIR=$MODDIR"

mkdir -p "$STATE_DIR" "$DATA_DIR" "$LOG_DIR" 2>/dev/null
chmod 0770 "$DATA_DIR" "$LOG_DIR" 2>/dev/null
chown 1017:1017 "$DATA_DIR" "$LOG_DIR" 2>/dev/null

# --- 首次部署配置 ---------------------------------------------------------
[ -f "$DATA_DIR/keybox.xml" ]   || cp -f "$MODDIR/keybox.xml"   "$DATA_DIR/keybox.xml"
[ -f "$DATA_DIR/injector.toml" ] || cp -f "$MODDIR/injector.toml" "$DATA_DIR/injector.toml"
chmod 0600 "$DATA_DIR/keybox.xml" "$DATA_DIR/injector.toml" 2>/dev/null
chown 1017:1017 "$DATA_DIR/keybox.xml" "$DATA_DIR/injector.toml" 2>/dev/null

rm -f "$STATE_DIR/restart.keymint" "$STATE_DIR/restart.injector" "$STATE_DIR/restart.all"

# --- 二进制权限 -----------------------------------------------------------
for f in keymint inject daemon daemon-injector; do
    if [ -f "$MODDIR/$f" ]; then
        chmod 0755 "$MODDIR/$f" 2>/dev/null
        chcon u:object_r:system_file:s0 "$MODDIR/$f" 2>/dev/null
    fi
done

# --- 守护进程管理 ---------------------------------------------------------
pid_matches_script() {
    pid=$1
    script=$2
    [ -r "/proc/$pid/cmdline" ] || return 1
    cmdline=$(tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null)
    echo "$cmdline" | grep -F "$script" >/dev/null 2>&1
}

start_daemon() {
    script=$1
    pidfile=$2
    if [ -f "$pidfile" ]; then
        pid=$(cat "$pidfile" 2>/dev/null)
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null && pid_matches_script "$pid" "$script"; then
            echo "already running: $script (pid $pid)"
            return 0
        fi
        rm -f "$pidfile"
    fi
    nohup sh "$script" >> "$LOG_DIR/daemon.log" 2>&1 &
    pid=$!
    echo "$pid" > "$pidfile"
    sleep 1
    if kill -0 "$pid" 2>/dev/null; then
        echo "started: $script (pid $pid)"
        return 0
    fi
    rm -f "$pidfile"
    echo "FAILED to start: $script"
    return 1
}

start_daemon "$MODDIR/daemon"          "$STATE_DIR/keymint-daemon.pid"
start_daemon "$MODDIR/daemon-injector" "$STATE_DIR/injector-daemon.pid"

echo "=== skroot_start.sh done ==="
