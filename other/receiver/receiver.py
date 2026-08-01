#!/usr/bin/env python3
"""
ETEST 图传接收端

功能：
1. 接收 H.264/RTP/UDP 视频并全屏显示。
2. 收到 TEST_START 后开始接收端录像。
3. 收到 TEST_DONE 后保留 0.5 秒尾帧再关闭文件。
4. 保存 1 秒预录，避免 CONTESTSTART ACK 到达稍晚而漏掉起步画面。
5. 发送端不保存视频，所有测试视频只保存在接收端。
"""

from __future__ import annotations

import queue
import re
import socket
import threading
import time
from collections import deque
from datetime import datetime
from pathlib import Path
from typing import Deque, Optional, Tuple

import cv2
import numpy as np


VIDEO_PORT = 5600
CONTROL_PORT = 5601

RECORD_DIR = Path.home() / "etest_videos"
RECORD_DIR.mkdir(parents=True, exist_ok=True)

STREAM_WIDTH = 960
STREAM_HEIGHT = 480
STREAM_FPS = 25.0

JITTER_LATENCY_MS = 80
PRE_ROLL_SECONDS = 1.0
POST_ROLL_SECONDS = 0.5
MAX_RECORD_SECONDS = 90.0

WINDOW_NAME = "ETEST Receiver"


GST_PIPELINE = (
    f'udpsrc port={VIDEO_PORT} '
    'caps="application/x-rtp,media=(string)video,'
    'encoding-name=(string)H264,payload=(int)96,'
    'clock-rate=(int)90000" ! '
    f'rtpjitterbuffer latency={JITTER_LATENCY_MS} '
    'drop-on-latency=true ! '
    'rtph264depay ! '
    'h264parse ! '
    'avdec_h264 ! '
    'videoconvert ! '
    'video/x-raw,format=BGR ! '
    'appsink sync=false drop=true max-buffers=2'
)


def safe_name(value: str) -> str:
    value = re.sub(r"[^0-9A-Za-z_.-]+", "_", value.strip())
    return value[:64] or "UNKNOWN"


class VideoReceiver:
    """后台解码线程。队列满时丢弃最旧帧，避免延迟不断累积。"""

    def __init__(self, pipeline: str) -> None:
        self.pipeline = pipeline
        self.frames: queue.Queue[Tuple[float, np.ndarray]] = queue.Queue(
            maxsize=8
        )
        self.running = threading.Event()
        self.running.set()
        self.online = threading.Event()
        self.thread = threading.Thread(
            target=self._worker,
            name="video-receiver",
            daemon=True,
        )

    def start(self) -> None:
        self.thread.start()

    def stop(self) -> None:
        self.running.clear()

    def _push_latest(self, item: Tuple[float, np.ndarray]) -> None:
        while self.running.is_set():
            try:
                self.frames.put_nowait(item)
                return
            except queue.Full:
                try:
                    self.frames.get_nowait()
                except queue.Empty:
                    return

    def _worker(self) -> None:
        while self.running.is_set():
            print("[VIDEO] Opening GStreamer stream...")
            capture = cv2.VideoCapture(
                self.pipeline,
                cv2.CAP_GSTREAMER,
            )

            if not capture.isOpened():
                self.online.clear()
                print("[VIDEO] Open failed; retrying in 1 second.")
                capture.release()
                time.sleep(1.0)
                continue

            self.online.set()
            print("[VIDEO] Stream connected.")

            while self.running.is_set():
                ok, frame = capture.read()
                if not ok or frame is None:
                    break

                self._push_latest((time.monotonic(), frame))

            self.online.clear()
            capture.release()
            print("[VIDEO] Stream interrupted; reconnecting.")
            time.sleep(0.3)


class ControlReceiver:
    """接收发送端的 TEST_START / TEST_DONE 控制消息。"""

    def __init__(self, port: int) -> None:
        self.port = port
        self.messages: queue.Queue[str] = queue.Queue(maxsize=64)
        self.running = threading.Event()
        self.running.set()
        self.thread = threading.Thread(
            target=self._worker,
            name="control-receiver",
            daemon=True,
        )

    def start(self) -> None:
        self.thread.start()

    def stop(self) -> None:
        self.running.clear()

    def _worker(self) -> None:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(("0.0.0.0", self.port))
        sock.settimeout(0.2)

        print(f"[CONTROL] Listening on UDP {self.port}")

        try:
            while self.running.is_set():
                try:
                    data, address = sock.recvfrom(1024)
                except socket.timeout:
                    continue
                except OSError as error:
                    print(f"[CONTROL] Socket error: {error}")
                    time.sleep(0.5)
                    continue

                message = data.decode("utf-8", errors="replace").strip()
                print(f"[CONTROL] {address[0]}: {message}")

                try:
                    self.messages.put_nowait(message)
                except queue.Full:
                    try:
                        self.messages.get_nowait()
                    except queue.Empty:
                        pass

                    try:
                        self.messages.put_nowait(message)
                    except queue.Full:
                        pass
        finally:
            sock.close()


class TestRecorder:
    def __init__(self) -> None:
        self.writer: Optional[cv2.VideoWriter] = None
        self.session_id = ""
        self.mode = ""
        self.base_stem = ""
        self.temp_path: Optional[Path] = None
        self.started_at = 0.0
        self.stop_deadline: Optional[float] = None
        self.pending_result = "DONE"
        self.frame_size = (STREAM_WIDTH, STREAM_HEIGHT)

    @property
    def active(self) -> bool:
        return self.writer is not None

    def start(
        self,
        session_id: str,
        mode: str,
        frame_size: Tuple[int, int],
        pre_roll: list[np.ndarray],
    ) -> bool:
        if self.active:
            self.stop("INTERRUPTED")

        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")[:-3]
        self.session_id = safe_name(session_id)
        self.mode = safe_name(mode)
        self.base_stem = (
            f"{timestamp}_{self.mode}_session-{self.session_id}"
        )
        self.temp_path = RECORD_DIR / f"{self.base_stem}_RECORDING.avi"
        self.frame_size = frame_size

        fourcc = cv2.VideoWriter_fourcc(*"MJPG")
        writer = cv2.VideoWriter(
            str(self.temp_path),
            fourcc,
            STREAM_FPS,
            self.frame_size,
        )

        if not writer.isOpened():
            print(f"[RECORD] Cannot create {self.temp_path}")
            writer.release()
            self._clear_state()
            return False

        self.writer = writer
        self.started_at = time.monotonic()
        self.stop_deadline = None
        self.pending_result = "DONE"

        for frame in pre_roll:
            self.write(frame)

        print(f"[RECORD] Started: {self.temp_path}")
        return True

    def write(self, frame: np.ndarray) -> None:
        if self.writer is None:
            return

        width, height = self.frame_size
        if frame.shape[1] != width or frame.shape[0] != height:
            frame = cv2.resize(
                frame,
                (width, height),
                interpolation=cv2.INTER_AREA,
            )

        self.writer.write(frame)

    def request_stop(self, session_id: str, result: str) -> None:
        if not self.active:
            return

        if safe_name(session_id) != self.session_id:
            print(
                "[RECORD] Ignoring TEST_DONE for another session: "
                f"{session_id}"
            )
            return

        if self.stop_deadline is None:
            self.stop_deadline = time.monotonic() + POST_ROLL_SECONDS
            self.pending_result = safe_name(result)
            print(
                f"[RECORD] DONE received; closing after "
                f"{POST_ROLL_SECONDS:.1f}s post-roll."
            )

    def update(self) -> None:
        if not self.active:
            return

        now = time.monotonic()

        if (
            self.stop_deadline is not None
            and now >= self.stop_deadline
        ):
            self.stop(self.pending_result)
            return

        if now - self.started_at >= MAX_RECORD_SECONDS:
            print("[RECORD] Safety timeout.")
            self.stop("TIMEOUT")

    def stop(self, result: str) -> None:
        if self.writer is None:
            self._clear_state()
            return

        self.writer.release()
        self.writer = None

        final_path = RECORD_DIR / (
            f"{self.base_stem}_{safe_name(result)}.avi"
        )

        if self.temp_path is not None and self.temp_path.exists():
            self.temp_path.replace(final_path)

        print(f"[RECORD] Saved: {final_path}")
        self._clear_state()

    def _clear_state(self) -> None:
        self.writer = None
        self.session_id = ""
        self.mode = ""
        self.base_stem = ""
        self.temp_path = None
        self.started_at = 0.0
        self.stop_deadline = None
        self.pending_result = "DONE"


def draw_status(
    frame: np.ndarray,
    video_online: bool,
    recorder: TestRecorder,
    rx_fps: float,
) -> np.ndarray:
    display = frame.copy()

    video_text = "VIDEO ONLINE" if video_online else "VIDEO WAITING"
    record_text = (
        f"REC {recorder.mode} / {recorder.session_id}"
        if recorder.active
        else "REC OFF"
    )

    cv2.putText(
        display,
        video_text,
        (20, 35),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.8,
        (255, 255, 255),
        2,
        cv2.LINE_AA,
    )
    cv2.putText(
        display,
        record_text,
        (20, 70),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.8,
        (255, 255, 255),
        2,
        cv2.LINE_AA,
    )
    cv2.putText(
        display,
        f"RX FPS {rx_fps:.1f}",
        (20, 105),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.8,
        (255, 255, 255),
        2,
        cv2.LINE_AA,
    )

    return display


def main() -> None:
    video = VideoReceiver(GST_PIPELINE)
    control = ControlReceiver(CONTROL_PORT)
    recorder = TestRecorder()

    video.start()
    control.start()

    pre_roll: Deque[Tuple[float, np.ndarray]] = deque()
    pending_start: Optional[Tuple[str, str]] = None

    last_frame = np.zeros(
        (STREAM_HEIGHT, STREAM_WIDTH, 3),
        dtype=np.uint8,
    )
    last_frame_time = 0.0

    fps_window_start = time.monotonic()
    fps_frames = 0
    rx_fps = 0.0

    cv2.namedWindow(WINDOW_NAME, cv2.WINDOW_NORMAL)
    cv2.setWindowProperty(
        WINDOW_NAME,
        cv2.WND_PROP_FULLSCREEN,
        cv2.WINDOW_FULLSCREEN,
    )

    try:
        while True:
            # 先处理完控制消息。发送端会重复发三份，下面按状态去重。
            while True:
                try:
                    message = control.messages.get_nowait()
                except queue.Empty:
                    break

                parts = message.split("|")
                command = parts[0]

                if command == "TEST_START" and len(parts) >= 3:
                    session_id = safe_name(parts[1])
                    mode = safe_name(parts[2])

                    if (
                        recorder.active
                        and recorder.session_id == session_id
                    ):
                        continue

                    if (
                        pending_start is not None
                        and pending_start[0] == session_id
                    ):
                        continue

                    pending_start = (session_id, mode)
                    print(
                        f"[CONTROL] Pending start: "
                        f"session={session_id}, mode={mode}"
                    )

                elif command == "TEST_DONE" and len(parts) >= 4:
                    session_id = safe_name(parts[1])
                    result = safe_name(parts[3])
                    recorder.request_stop(session_id, result)

            got_frame = False

            try:
                frame_time, frame = video.frames.get(timeout=0.02)
                got_frame = True
            except queue.Empty:
                frame_time, frame = 0.0, None

            if got_frame and frame is not None:
                # TEST_START 到来时先写入此前 1 秒预录，再写当前帧。
                if pending_start is not None:
                    session_id, mode = pending_start
                    width = int(frame.shape[1])
                    height = int(frame.shape[0])
                    recorder.start(
                        session_id,
                        mode,
                        (width, height),
                        [item[1] for item in pre_roll],
                    )
                    pending_start = None

                recorder.write(frame)

                last_frame = frame
                last_frame_time = frame_time

                pre_roll.append((frame_time, frame))
                cutoff = frame_time - PRE_ROLL_SECONDS
                while pre_roll and pre_roll[0][0] < cutoff:
                    pre_roll.popleft()

                fps_frames += 1
                elapsed = time.monotonic() - fps_window_start
                if elapsed >= 1.0:
                    rx_fps = fps_frames / elapsed
                    fps_frames = 0
                    fps_window_start = time.monotonic()

            recorder.update()

            online = (
                video.online.is_set()
                and time.monotonic() - last_frame_time < 1.0
            )

            display = draw_status(
                last_frame,
                online,
                recorder,
                rx_fps,
            )
            cv2.imshow(WINDOW_NAME, display)

            key = cv2.waitKey(1) & 0xFF
            if key in (27, ord("q"), ord("Q")):
                break

            # 手动兜底：R 开始/停止录像。
            if key in (ord("r"), ord("R")):
                if recorder.active:
                    recorder.stop("MANUAL")
                else:
                    pending_start = (
                        str(int(time.time() * 1000)),
                        "MANUAL",
                    )

    finally:
        recorder.stop("PROGRAM_EXIT")
        video.stop()
        control.stop()
        cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
