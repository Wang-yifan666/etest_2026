#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C

IFACE="${IFACE:-wlan1}"
CON_NAME="${CON_NAME:-etest-adhoc}"
SSID="${SSID:-ETEST-DIRECT}"
BAND="${BAND:-a}"
CHANNEL="${CHANNEL:-36}"
LOCAL_CIDR="${LOCAL_CIDR:-192.168.60.2/24}"
LOCAL_IP="${LOCAL_IP:-192.168.60.2}"
RECEIVER_IP="${RECEIVER_IP:-192.168.60.1}"
SENDER_APP="${SENDER_APP:-$HOME/etest_2026/build/etest_2026}"
WAIT_SECONDS="${WAIT_SECONDS:-30}"

log(){ echo "[TX-ADHOC] $*"; }
die(){ echo "[TX-ADHOC] ERROR: $*" >&2; exit 1; }
need(){ command -v "$1" >/dev/null 2>&1 || die "缺少命令: $1"; }

check(){
    need nmcli; need ip; need iw; need ping
    ip link show "$IFACE" >/dev/null 2>&1 || die "找不到接口 $IFACE"
    local adhoc
    adhoc="$(nmcli -t -f WIFI-PROPERTIES.ADHOC device show "$IFACE" 2>/dev/null || true)"
    grep -q ':yes$' <<<"$adhoc" || die "$IFACE 未报告 Ad-Hoc 支持: ${adhoc:-无输出}"
    [[ -x "$SENDER_APP" ]] || die "发送程序不存在或不可执行: $SENDER_APP"
}

setup_link(){
    log "配置 $IFACE, SSID=$SSID, band=$BAND, channel=$CHANNEL"
    sudo nmcli connection down "$CON_NAME" >/dev/null 2>&1 || true
    sudo nmcli connection delete "$CON_NAME" >/dev/null 2>&1 || true

    sudo nmcli connection add type wifi ifname "$IFACE" con-name "$CON_NAME" ssid "$SSID" >/dev/null
    sudo nmcli connection modify "$CON_NAME"         802-11-wireless.mode adhoc         802-11-wireless.band "$BAND"         802-11-wireless.channel "$CHANNEL"         802-11-wireless.cloned-mac-address permanent         ipv4.method manual         ipv4.addresses "$LOCAL_CIDR"         ipv4.never-default yes         ipv6.method disabled         connection.autoconnect yes         connection.autoconnect-priority 100

    sudo nmcli connection up "$CON_NAME" || die "Ad-Hoc 启动失败；可试 BAND=bg CHANNEL=6"
    sudo iw dev "$IFACE" set power_save off >/dev/null 2>&1 || true
    ip -4 -brief address show dev "$IFACE"
    ip route get "$RECEIVER_IP" || die "没有到接收端的路由"
}

wait_rx(){
    log "等待接收端 $RECEIVER_IP，最多 ${WAIT_SECONDS}s"
    for ((i=1; i<=WAIT_SECONDS; i++)); do
        if ping -I "$LOCAL_IP" -c 1 -W 1 "$RECEIVER_IP" >/dev/null 2>&1; then
            log "接收端已连通"
            return 0
        fi
        sleep 1
    done
    die "无法连接接收端；检查两端 SSID/频段/信道"
}

status(){
    nmcli -f GENERAL.DEVICE,GENERAL.STATE,GENERAL.CONNECTION device show "$IFACE"
    ip -4 -brief address show dev "$IFACE"
    iw dev "$IFACE" link || true
    ip route get "$RECEIVER_IP" || true
}

case "${1:-run}" in
    run)
        check
        setup_link
        wait_rx
        log "启动发送端: $SENDER_APP"
        log "请确认 C++ 中 receiver_ip = $RECEIVER_IP"
        exec "$SENDER_APP"
        ;;
    setup)
        check
        setup_link
        ;;
    test)
        check
        setup_link
        wait_rx
        ping -I "$LOCAL_IP" -c 10 "$RECEIVER_IP"
        ;;
    status)
        need nmcli; need ip; need iw
        status
        ;;
    stop)
        need nmcli
        sudo nmcli connection down "$CON_NAME" >/dev/null 2>&1 || true
        ;;
    *)
        echo "用法: $0 [run|setup|test|status|stop]"
        echo "示例: IFACE=wlan1 BAND=a CHANNEL=36 $0 run"
        exit 2
        ;;
esac
