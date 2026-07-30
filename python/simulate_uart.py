#!/usr/bin/env python3
"""下位机串口模拟器（工程通信协议 V5.2）

支持 V5.2 协议：
  - 三步握手 PING → PROTO?,<major>,<minor> → CAPS?
  - M000X 题目选择
  - VSESSION 握手
  - BALL 报文接收 (V5 Simple)
  - CONTESTSTART / CONTESTSTOP
  - DONE 发送

使用方式：
  # PTY 模式（推荐，WSL 下测试无需物理串口）
  python3 python/simulate_uart.py --pty --scenario normal

  # 连接到真实串口
  python3 python/simulate_uart.py --port /dev/ttyUSB0 --scenario normal

  # 查看所有场景
  python3 python/simulate_uart.py --list-scenarios

场景说明：
  normal          正常流程：BOOT,OK → 三步握手 → M000X 选题 →
                  VSESSION ACK → 接收 BALL → CONTESTSTART ACK →
                  运行 → 手动按 Enter 发 DONE
  no_response     完全不响应（模拟掉线）
  wrong_version   PROTO 返回错误版本
  error_warn      正常 + 随机 WARN/ERR
  restart         运行中期发送 BOOT,OK 模拟重启

依赖：
  pip install pyserial
"""

import argparse
import os
import random
import sys
import time
import datetime

try:
    import serial
except ImportError:
    print("[FATAL] pyserial 未安装，请执行: pip install pyserial")
    sys.exit(1)


# =============================================================================
# V5 简化版协议处理
# =============================================================================

def handle_message(line, scenario_state):
    """处理一条上位机消息，返回应回复的消息列表。"""
    line = line.strip()
    if not line:
        return []

    scenario = scenario_state.get("scenario", "normal")

    if scenario == "no_response":
        return []

    if scenario == "wrong_version":
        return _handle_wrong_version(line)

    if scenario == "error_warn":
        return _handle_error_warn(line)

    if scenario == "restart":
        return _handle_restart(line, scenario_state)

    return _handle_normal(line, scenario_state)


def _handle_normal(line, state):
    """正常场景：V5.2 协议回复。"""
    # ── 握手 ──
    if line == "PING":
        return ["OK,PING"]

    if line == "PROTO?":
        return ["PROTO,5,2"]

    if line == "CAPS?":
        return ["CAPS,MOTION=4,STATUS=4,TUNE=5,BALL=2,ROD=1,CONTEST=1"]

    # ── VSESSION 握手 ──
    if line.startswith("VSESSION,"):
        parts = line.split(",")
        if len(parts) >= 4:
            session_id = parts[1]
            state["session_id"] = session_id
            print(f"[SIM] VSESSION session={session_id}", file=sys.stderr)
            return [f"OK,VSESSION,{session_id}"]

    # ── BALL 报文（只记录，不逐条 ACK）──
    if line.startswith("BALL,"):
        parts = line.split(",")
        if len(parts) >= 6:
            session = parts[1]
            seq = parts[2]
            capture_ms = parts[3]
            age_ms = parts[4]
            pos = parts[5]
            conf = parts[6] if len(parts) > 6 else "?"
            status = parts[7] if len(parts) > 7 else "?"
            # 节流打印：每 30 条打印一次
            seq_int = int(seq) if seq.isdigit() else 0
            if seq_int % 30 == 0:
                print(f"[SIM] BALL seq={seq} pos={pos} conf={conf} status={status} age={age_ms}ms",
                      file=sys.stderr)
        return []

    # ── CONTESTSTART ──
    if line.startswith("CONTESTSTART,"):
        parts = line.split(",")
        mode = parts[1] if len(parts) > 1 else "?"
        state["active_mode"] = mode
        state["contest_running"] = True
        state["contest_start_time"] = time.time()
        state["ball_count"] = 0
        print(f"\n[SIM] ===== CONTESTSTART received: mode={mode} =====", file=sys.stderr)
        print(f"[SIM] Press ENTER to send DONE (simulate contest completion)", file=sys.stderr)
        return [f"OK,CONTESTSTART,{mode},ACCEPTED"]

    # ── CONTESTSTOP ──
    if line == "CONTESTSTOP":
        mode = state.get("active_mode", "?")
        state["contest_running"] = False
        print(f"\n[SIM] ===== CONTESTSTOP received =====", file=sys.stderr)
        return ["OK,CONTESTSTOP"]

    return []


def _handle_wrong_version(line):
    """返回错误的协议版本。"""
    if line == "PING":
        return ["OK,PING"]
    if line == "PROTO?":
        return ["PROTO,3,0"]  # 故意返回错误的主版本
    if line == "CAPS?":
        return ["CAPS,MOTION=4,STATUS=4,TUNE=5,BALL=2,ROD=1,CONTEST=1"]
    return _handle_normal(line, {})


def _handle_error_warn(line):
    """正常响应 + 随机 WARN/ERR。"""
    responses = _handle_normal(line, {})

    if random.random() < 0.15:
        warn_msgs = [
            "WARN,UART,NOISE,BUS_BURST,",
            "WARN,POWER,LOW_BATTERY,3.7V,",
        ]
        responses.append(random.choice(warn_msgs))

    if random.random() < 0.05:
        err_msgs = [
            "ERR,UART,NOISE,DATA_DROP,",
            "ERR,MOTOR,OVERCURRENT,LEFT,2.3A",
        ]
        responses.append(random.choice(err_msgs))

    return responses


def _handle_restart(line, state):
    """模拟重启场景。"""
    reply_count = state.get("restart_reply_count", 0)
    state["restart_reply_count"] = reply_count + 1

    if reply_count > 20 and random.random() < 0.08:
        state["restart_reply_count"] = 0
        state["session_id"] = None
        state["contest_running"] = False
        print(f"\n[SIM] ===== SIMULATED RESTART (BOOT,OK) =====\n", file=sys.stderr)
        return ["BOOT,OK,99,1.0.1"]

    return _handle_normal(line, state)


# =============================================================================
# 串口通信
# =============================================================================

def open_serial(port, baudrate=115200):
    """打开真实串口。"""
    ser = serial.Serial(
        port=port,
        baudrate=baudrate,
        bytesize=serial.EIGHTBITS,
        parity=serial.PARITY_NONE,
        stopbits=serial.STOPBITS_ONE,
        timeout=0.1,
    )
    print(f"[INFO] 串口已打开: {port} @ {baudrate}")
    return ser


def open_pty():
    """创建伪终端对，返回 (controller_ser, slave_name)。"""
    import pty
    import tty

    master_fd, slave_fd = pty.openpty()
    tty.setraw(master_fd)
    slave_name = os.ttyname(slave_fd)

    # 设置为非阻塞
    import fcntl
    flags = fcntl.fcntl(master_fd, fcntl.F_GETFL)
    fcntl.fcntl(master_fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)

    class PtySerial:
        def __init__(self, fd, name):
            self.fd = fd
            self.name = name
            self._buf = b""

        def write(self, data):
            if isinstance(data, str):
                data = data.encode("utf-8")
            try:
                return os.write(self.fd, data)
            except OSError:
                return 0

        def read(self, size=4096):
            try:
                data = os.read(self.fd, size)
                return data if data else b""
            except BlockingIOError:
                return b""

        def close(self):
            os.close(self.fd)

    return PtySerial(master_fd, slave_name), slave_name


def _send_line(ser, line, ending="\r\n"):
    """发送一行文本。"""
    data = line + ending
    if isinstance(data, str):
        data = data.encode("utf-8")
    try:
        ser.write(data)
    except Exception as e:
        print(f"[ERROR] write failed: {e}", file=sys.stderr)


# =============================================================================
# 主循环
# =============================================================================

def run_loop(ser, scenario):
    """主循环：接收 → 处理 → 回复。"""
    state = {
        "scenario": scenario,
        "restart_reply_count": 0,
        "session_id": None,
        "active_mode": None,
        "contest_running": False,
        "contest_start_time": 0.0,
        "ball_count": 0,
        "caps_received": 0.0,
    }

    line_ending = "\r\n"
    rx_buf = ""

    # 下位机启动后主动发送 BOOT,OK
    boot_line = "BOOT,OK,84,1.0.1"
    _send_line(ser, boot_line, line_ending)
    print(f"[TX] {boot_line}")

    print(f"[INFO] 模拟器就绪，等待上位机连接...")
    print(f"[INFO] 协议版本: V5.2 (PROTO,5,2)")
    print(f"[INFO] Press Ctrl+C to exit\n")

    last_status_time = time.time()
    ball_count = 0
    mission_sent = False
    mission_delay = 0.5  # 握手完成后延迟发送 M000X (秒)

    while True:
        # 握手完成后自动发送 M000X 选题
        if not mission_sent and state.get("caps_received") > 0:
            caps_time = state.get("caps_received", 0)
            if time.time() - caps_time > mission_delay:
                mission_line = "M0004"  # H5 题目
                _send_line(ser, mission_line, line_ending)
                print(f"[TX] {mission_line}")
                mission_sent = True

        # 非阻塞读取串口数据
        try:
            data = ser.read(4096)
        except Exception as e:
            print(f"[ERROR] 串口读取失败: {e}")
            time.sleep(1)
            continue

        if data:
            if isinstance(data, bytes):
                data = data.decode("utf-8", errors="replace")
            rx_buf += data

            # 处理所有完整行
            while "\n" in rx_buf:
                idx = rx_buf.index("\n")
                line = rx_buf[:idx]
                rx_buf = rx_buf[idx + 1:]

                if line.endswith("\r"):
                    line = line[:-1]

                if not line:
                    continue

                print(f"[RX] {line}")

                # 检测 CAPS 响应时间戳，用于触发 M000X
                if line.startswith("CAPS,"):
                    state["caps_received"] = time.time()

                responses = handle_message(line, state)

                for resp in responses:
                    if isinstance(resp, tuple) and resp[0] == "RAW":
                        ser.write(resp[1].encode("utf-8") if isinstance(resp[1], str) else resp[1])
                        print(f"[TX RAW] {resp[1]!r}")
                    else:
                        _send_line(ser, resp, line_ending)
                        print(f"[TX] {resp}")

            if len(rx_buf) > 4096:
                print("[WARN] 接收缓冲过大，清空")
                rx_buf = ""

        # 状态节流打印
        now = time.time()
        if state.get("contest_running") and now - last_status_time > 5.0:
            elapsed = now - state.get("contest_start_time", now)
            print(f"[SIM] Contest running: {elapsed:.0f}s elapsed, mode={state.get('active_mode', '?')}", file=sys.stderr)
            last_status_time = now

        time.sleep(0.01)


# =============================================================================
# 入口
# =============================================================================

SCENARIOS = [
    "normal",
    "no_response",
    "wrong_version",
    "error_warn",
    "restart",
]


def main():
    parser = argparse.ArgumentParser(
        description="下位机串口模拟器（工程通信协议 V5 简化版）"
    )
    parser.add_argument(
        "--port", default="/dev/ttyACM0",
        help="串口设备路径（默认 /dev/ttyACM0）"
    )
    parser.add_argument(
        "--baudrate", type=int, default=115200,
        help="波特率（默认 115200）"
    )
    parser.add_argument(
        "--scenario", default="normal",
        choices=SCENARIOS,
        help="模拟场景"
    )
    parser.add_argument(
        "--pty", action="store_true",
        help="使用伪终端模式（不需要物理串口，WSL 下推荐）"
    )
    parser.add_argument(
        "--list-scenarios", action="store_true",
        help="列出所有可用场景"
    )
    args = parser.parse_args()

    if args.list_scenarios:
        print("可用场景：")
        for s in SCENARIOS:
            desc = {
                "normal": "正常流程：握手 → BALL 接收 → CONTESTSTART → 运行",
                "no_response": "完全不响应（模拟 MCU 掉线）",
                "wrong_version": "PROTO 返回错误版本号 PROTO,3",
                "error_warn": "正常 + 随机插入 WARN/ERR 消息",
                "restart": "正常运行 + 随机发送 BOOT,OK 模拟重启",
            }.get(s, "")
            print(f"  {s:<18} {desc}")
        return

    if args.pty:
        ser, slave_name = open_pty()
        print(f"[INFO] 伪终端已创建")
        print(f"[INFO] 上位机请连接设备: {slave_name}")
        print(f"[INFO] 提示: 修改 config/uart.toml 中 uart.device = \"{slave_name}\"\n")
    else:
        ser = open_serial(args.port, args.baudrate)

    try:
        run_loop(ser, args.scenario)
    except KeyboardInterrupt:
        print("\n[INFO] 模拟器退出 (Ctrl+C)")
    finally:
        ser.close()
        print("[INFO] 串口已关闭")


if __name__ == "__main__":
    main()