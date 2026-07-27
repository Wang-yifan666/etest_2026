#!/usr/bin/env python3
"""模拟下位机 — 协议编解码与测试"""

import argparse
import struct
import sys
import time
import random

# CRC16-CCITT 查表 (多项式 0x1021)
CRC16_TABLE = [
    0x0000,0x1021,0x2042,0x3063,0x4084,0x50A5,0x60C6,0x70E7,
    0x8108,0x9129,0xA14A,0xB16B,0xC18C,0xD1AD,0xE1CE,0xF1EF,
    0x1231,0x0210,0x3273,0x2252,0x52B5,0x4294,0x72F7,0x62D6,
    0x9339,0x8318,0xB37B,0xA35A,0xD3BD,0xC39C,0xF3FF,0xE3DE,
    0x2462,0x3443,0x0420,0x1401,0x64E6,0x74C7,0x44A4,0x5485,
    0xA56A,0xB54B,0x8528,0x9509,0xE5EE,0xF5CF,0xC5AC,0xD58D,
    0x3653,0x2672,0x1611,0x0630,0x76D7,0x66F6,0x5695,0x46B4,
    0xB75B,0xA77A,0x9719,0x8738,0xF7DF,0xE7FE,0xD79D,0xC7BC,
    0x48C4,0x58E5,0x6886,0x78A7,0x0840,0x1861,0x2802,0x3823,
    0xC9CC,0xD9ED,0xE98E,0xF9AF,0x8948,0x9969,0xA90A,0xB92B,
    0x5AF5,0x4AD4,0x7AB7,0x6A96,0x1A71,0x0A50,0x3A33,0x2A12,
    0xDBFD,0xCBDC,0xFBBF,0xEB9E,0x9B79,0x8B58,0xBB3B,0xAB1A,
    0x6CA6,0x7C87,0x4CE4,0x5CC5,0x2C22,0x3C03,0x0C60,0x1C41,
    0xEDAE,0xFD8F,0xCDEC,0xDDCD,0xAD2A,0xBD0B,0x8D68,0x9D49,
    0x7E97,0x6EB6,0x5ED5,0x4EF4,0x3E13,0x2E32,0x1E51,0x0E70,
    0xFF9F,0xEFBE,0xDFDD,0xCFFC,0xBF1B,0xAF3A,0x9F59,0x8F78,
    0x9188,0x81A9,0xB1CA,0xA1EB,0xD10C,0xC12D,0xF14E,0xE16F,
    0x1080,0x00A1,0x30C2,0x20E3,0x5004,0x4025,0x7046,0x6067,
    0x83B9,0x9398,0xA3FB,0xB3DA,0xC33D,0xD31C,0xE37F,0xF35E,
    0x02B1,0x1290,0x22F3,0x32D2,0x4235,0x5214,0x6277,0x7256,
    0xB5EA,0xA5CB,0x95A8,0x8589,0xF56E,0xE54F,0xD52C,0xC50D,
    0x34E2,0x24C3,0x14A0,0x0481,0x7466,0x6447,0x5424,0x4405,
    0xA7DB,0xB7FA,0x8799,0x97B8,0xE75F,0xF77E,0xC71D,0xD73C,
    0x26D3,0x36F2,0x0691,0x16B0,0x6657,0x7676,0x4615,0x5634,
    0xD94C,0xC96D,0xF90E,0xE92F,0x99C8,0x89E9,0xB98A,0xA9AB,
    0x5844,0x4865,0x7806,0x6827,0x18C0,0x08E1,0x3882,0x28A3,
    0xCB7D,0xDB5C,0xEB3F,0xFB1E,0x8BF9,0x9BD8,0xABBB,0xBB9A,
    0x4A75,0x5A54,0x6A37,0x7A16,0x0AF1,0x1AD0,0x2AB3,0x3A92,
    0xFD2E,0xED0F,0xDD6C,0xCD4D,0xBDAA,0xAD8B,0x9DE8,0x8DC9,
    0x7C26,0x6C07,0x5C64,0x4C45,0x3CA2,0x2C83,0x1CE0,0x0CC1,
    0xEF1F,0xFF3E,0xCF5D,0xDF7C,0xAF9B,0xBFBA,0x8FD9,0x9FF8,
    0x6E17,0x7E36,0x4E55,0x5E74,0x2E93,0x3EB2,0x0ED1,0x1EF0,
]


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc = ((crc << 8) ^ CRC16_TABLE[((crc >> 8) ^ b) & 0xFF]) & 0xFFFF
    return crc


def encode_message(version: int, msg_type: str, seq: int, payload: str) -> str:
    body = f"{msg_type},{seq},{payload}"
    crc = crc16(body.encode())
    return f"@{version},{body},{crc:04X}"


def decode_frame(frame: str):
    """返回 (version, type, seq, payload) 或 None"""
    if not frame.startswith("@"):
        return None
    parts = frame[1:].split(",")
    if len(parts) != 5:
        return None
    version_str, msg_type, seq_str, payload, crc_str = parts
    try:
        version = int(version_str)
        seq = int(seq_str)
    except ValueError:
        return None
    crc_body = f"{msg_type},{seq_str},{payload}"
    expected = crc16(crc_body.encode())
    try:
        actual = int(crc_str, 16)
    except ValueError:
        return None
    if expected != actual:
        return None
    return version, msg_type, seq, payload


def main():
    parser = argparse.ArgumentParser(description="模拟下位机")
    parser.add_argument("--no-reply", action="store_true", help="不回复任何消息")
    parser.add_argument("--bad-crc", action="store_true", help="回复错误 CRC")
    parser.add_argument("--delay-ms", type=int, default=0, help="回复延迟(ms)")
    parser.add_argument("--half-packet", action="store_true", help="半包发送")
    parser.add_argument("--restart", action="store_true", help="模拟下位机重启")
    parser.add_argument("--random-bad", action="store_true", help="随机发送坏包")
    args = parser.parse_args()

    print("[SIMULATOR] started", file=sys.stderr)
    if args.no_reply:
        print("[SIMULATOR] no-reply mode", file=sys.stderr)

    restart_count = 0
    lines_since_restart = 0

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue

        lines_since_restart += 1

        if args.restart and lines_since_restart > 20:
            restart_count += 1
            print(f"[SIMULATOR] restart #{restart_count}", file=sys.stderr)
            lines_since_restart = 0
            continue

        decoded = decode_frame(line)
        if decoded is None:
            print(f"[SIMULATOR] bad frame: {line}", file=sys.stderr)
            continue

        version, msg_type, seq, payload = decoded
        print(f"[SIMULATOR] RECV: {msg_type} seq={seq} payload={payload}", file=sys.stderr)

        if args.no_reply:
            continue

        if args.delay_ms > 0:
            time.sleep(args.delay_ms / 1000.0)

        if msg_type == "HELLO":
            reply = encode_message(1, "READY", seq, "OK")
            if args.bad_crc:
                reply = reply[:-1] + "0"
            print(reply, flush=True)
        elif msg_type == "PING":
            reply = encode_message(1, "PONG", seq, "0")
            if args.bad_crc and random.random() < 0.5:
                reply = reply[:-1] + "0"
            print(reply, flush=True)
        elif msg_type in ("TARGET", "LOST", "ERROR"):
            print(f"[SIMULATOR] OK: {msg_type}", file=sys.stderr)

        if args.half_packet and random.random() < 0.2:
            print(f"[SIMULATOR] half-packet mode active", file=sys.stderr)
            # 发送一半然后重新发送完整
            time.sleep(0.1)

        if args.random_bad and random.random() < 0.1:
            print("BAD_FRAME_WITHOUT_AT\n", flush=True)
            print(f"[SIMULATOR] sent random bad frame", file=sys.stderr)

    print("[SIMULATOR] stopped", file=sys.stderr)


if __name__ == "__main__":
    main()