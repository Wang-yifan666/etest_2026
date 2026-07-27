#!/usr/bin/env python3
"""下位机串口模拟器（工程通信协议 V4 专用）

支持场景：
  normal         正常响应（BOOT,OK → PING→OK,PING → PROTO?→PROTO,4）
  boot_ok_only   只发送 BOOT,OK 后无其他响应
  no_response    完全不响应（模拟掉线）
  wrong_version  返回错误的协议版本 PROTO,3
  error_warn     随机发送 WARN/ERR 消息
  noise          随机发送乱码/噪声
  restart        运行中期发送 BOOT,OK 模拟重启
  half_packet    在正常流中插入半包（无换行符）
  multi_per_line 一行多个消息（模拟粘包）
  lf_only        只使用 \n（不使用 \r\n）

协议说明：
  - 纯文本协议
  - 以 \r\n 或 \n 为一条消息
  - 所有消息使用 ASCII 文本
  - 字段之间使用英文逗号分隔
  - 不使用帧头、CRC 等二进制协议特性

使用方式：
  python simulate_uart.py --port /dev/ttyUSB0 --scenario normal
  python simulate_uart.py --port /tmp/virtual_uart --scenario wrong_version
  python simulate_uart.py --pty --scenario normal

依赖：
  pip install pyserial
"""

import argparse
import os
import random
import sys
import time

try:
    import serial
except ImportError:
    print("[FATAL] pyserial 未安装，请执行: pip install pyserial")
    sys.exit(1)


# =============================================================================
# 消息处理（V4 协议）
# =============================================================================

def handle_message(line, scenario_state):
    """处理一条上位机消息，返回应回复的消息列表。"""
    line = line.strip()

    if not line:
        return []

    scenario = scenario_state.get("scenario", "normal")

    if scenario == "no_response":
        return []

    if scenario == "normal":
        return _handle_normal(line)

    if scenario == "boot_ok_only":
        return []

    if scenario == "wrong_version":
        return _handle_wrong_version(line)

    if scenario == "error_warn":
        return _handle_error_warn(line)

    if scenario == "noise":
        return _handle_noise(line)

    if scenario == "restart":
        return _handle_restart(line, scenario_state)

    if scenario == "half_packet":
        return _handle_half_packet(line, scenario_state)

    if scenario == "multi_per_line":
        return _handle_multi_per_line(line)

    if scenario == "lf_only":
        return _handle_lf_only(line, scenario_state)

    return _handle_normal(line)


def _handle_normal(line):
    """正常场景：按 V4 协议回复。"""
    if line == "PING":
        return ["OK,PING"]

    if line == "PROTO?":
        return ["PROTO,4"]

    if line.startswith("TARGET,"):
        parts = line.split(",")
        if len(parts) == 6:
            try:
                seq = int(parts[1])
                x = float(parts[2])
                y = float(parts[3])
                angle = float(parts[4])
                conf = float(parts[5])
                print(f"[SIM] TARGET seq={seq} pos=({x},{y}) "
                      f"angle={angle} conf={conf}", file=sys.stderr)
            except ValueError:
                print(f"[SIM] TARGET parse error: {line}", file=sys.stderr)
        else:
            print(f"[SIM] TARGET wrong field count: {len(parts)}",
                  file=sys.stderr)
        return []  # 不逐条回复视觉消息

    if line.startswith("LOST,"):
        parts = line.split(",")
        if len(parts) == 2:
            try:
                seq = int(parts[1])
                print(f"[SIM] LOST seq={seq}", file=sys.stderr)
            except ValueError:
                print(f"[SIM] LOST parse error: {line}", file=sys.stderr)
        return []

    # 其他命令：静默忽略
    return []


def _handle_wrong_version(line):
    """返回错误的协议版本。"""
    if line == "PING":
        return ["OK,PING"]

    if line == "PROTO?":
        return ["PROTO,3"]  # 故意返回 3 而不是 4

    return _handle_normal(line)


def _handle_error_warn(line):
    """正常响应 + 随机 WARN/ERR。"""
    responses = _handle_normal(line)

    if random.random() < 0.2:
        warn_msgs = [
            "WARN,UART,NOISE,BUS_BURST,",
            "WARN,UART,NOISE,OVERRUN,",
            "WARN,POWER,LOW_BATTERY,3.7V,",
        ]
        responses.append(random.choice(warn_msgs))

    if random.random() < 0.1:
        err_msgs = [
            "ERR,UART,NOISE,DATA_DROP,",
            "ERR,MOTOR,OVERCURRENT,LEFT,2.3A",
            "ERR,IMU,SPI_FAIL,,",
        ]
        responses.append(random.choice(err_msgs))

    return responses


def _handle_noise(line):
    """随机插入乱码/噪声。"""
    responses = _handle_normal(line)

    if random.random() < 0.3:
        noise = "".join(
            chr(random.randint(0, 255)) for _ in range(random.randint(1, 20))
        )
        responses.append(noise)

    return responses


def _handle_restart(line, state):
    reply_count = state.get("restart_reply_count", 0)

    if reply_count > 10 and random.random() < 0.1:
        state["restart_reply_count"] = 0
        return ["BOOT,OK"]

    state["restart_reply_count"] = reply_count + 1
    return _handle_normal(line)


def _handle_half_packet(line, state):
    """在正常响应中偶尔插入无换行符的半包数据。"""
    responses = _handle_normal(line)

    half_count = state.get("half_count", 0)
    state["half_count"] = half_count + 1

    if half_count % 5 == 3:
        half_data = "TARGET,99,320.00,240.00,10.00,0.950"
        responses.append(("RAW", half_data))

    return responses


def _handle_multi_per_line(line):
    """多消息粘包（一次发送多条完整消息）。"""
    responses = _handle_normal(line)

    if line == "PING" and random.random() < 0.3:
        extra = [
            "OK,PING",
            "STATUS,IMU.YAW,45.2",
        ]
        return extra

    return responses


def _handle_lf_only(line, state):
    """故意只使用 \n 作为行结束符。"""
    responses = _handle_normal(line)
    state["use_crlf"] = False
    return responses


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

    class PtySerial:
        def __init__(self, fd, name):
            self.fd = fd
            self.name = name
            self._buf = b""

        def write(self, data):
            if isinstance(data, str):
                data = data.encode("utf-8")
            return os.write(self.fd, data)

        def read(self, size=1024):
            try:
                return os.read(self.fd, size)
            except BlockingIOError:
                return b""

        def close(self):
            os.close(self.fd)

    return PtySerial(master_fd, slave_name), slave_name


def run_loop(ser, scenario, use_crlf=True):
    """主循环：接收 → 处理 → 回复。"""
    state = {
        "scenario": scenario,
        "restart_reply_count": 0,
        "half_count": 0,
        "use_crlf": True,
    }

    line_ending = "\r\n"
    rx_buf = ""

    # 下位机启动后主动发送 BOOT,OK
    boot_line = "BOOT,OK"
    _send_line(ser, boot_line, line_ending)
    print(f"[TX] {boot_line!r}")

    print(f"[INFO] 场景: {scenario}, 行结束符: CRLF={use_crlf}")
    print("[INFO] 等待上位机数据...\n")

    while True:
        try:
            data = ser.read(4096)
        except Exception as e:
            print(f"[ERROR] 串口读取失败: {e}")
            time.sleep(1)
            continue

        if not data:
            time.sleep(0.01)
            continue

        if isinstance(data, bytes):
            data = data.decode("utf-8", errors="replace")

        rx_buf += data

        while "\n" in rx_buf:
            idx = rx_buf.index("\n")
            line = rx_buf[:idx]
            rx_buf = rx_buf[idx + 1:]

            if line.endswith("\r"):
                line = line[:-1]

            if not line:
                continue

            print(f"[RX] {line!r}")

            responses = handle_message(line, state)

            for resp in responses:
                if isinstance(resp, tuple) and resp[0] == "RAW":
                    raw_data = resp[1]
                    if isinstance(raw_data, str):
                        raw_data = raw_data.encode("utf-8")
                    ser.write(raw_data)
                    print(f"[TX RAW] {raw_data!r}")
                else:
                    ending = line_ending
                    if not state.get("use_crlf", True):
                        ending = "\n"
                    _send_line(ser, resp, ending)
                    print(f"[TX] {resp!r}")

            if len(rx_buf) > 4096:
                print("[WARN] 接收缓冲过大，清空")
                rx_buf = ""


def _send_line(ser, line, ending="\r\n"):
    """发送一行文本。"""
    data = line + ending
    if isinstance(data, str):
        data = data.encode("utf-8")
    ser.write(data)


# =============================================================================
# 入口
# =============================================================================

SCENARIOS = [
    "normal",
    "boot_ok_only",
    "no_response",
    "wrong_version",
    "error_warn",
    "noise",
    "restart",
    "half_packet",
    "multi_per_line",
    "lf_only",
]


def main():
    parser = argparse.ArgumentParser(
        description="下位机串口模拟器（工程通信协议 V4）"
    )
    parser.add_argument(
        "--port", default="/dev/ttyUSB0",
        help="串口设备路径（默认 /dev/ttyUSB0）"
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
        help="使用伪终端模式（不需要物理串口）"
    )
    parser.add_argument(
        "--list-scenarios", action="store_true",
        help="列出所有可用场景"
    )
    args = parser.parse_args()

    if args.list_scenarios:
        print("可用场景：")
        for s in SCENARIOS:
            print(f"  {s}")
        return

    if args.pty:
        ser, slave_name = open_pty()
        print(f"[INFO] 伪终端已创建，上位机请连接: {slave_name}")
    else:
        ser = open_serial(args.port, args.baudrate)

    try:
        run_loop(ser, args.scenario)
    except KeyboardInterrupt:
        print("\n[INFO] 模拟器退出")
    finally:
        ser.close()
        print("[INFO] 串口已关闭")


if __name__ == "__main__":
    main()