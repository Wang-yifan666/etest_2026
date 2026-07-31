"""
后台线程摄像头读取类。

不从 GUI 主线程调用 cap.read()，避免摄像头阻塞导致界面卡死。
"""

from __future__ import annotations

import threading
import time
from typing import Optional

import cv2
import numpy as np


class CalibrationCamera:
    def __init__(
        self,
        camera_index: int = 0,
        *,
        width: int = 1280,
        height: int = 640,
        fps: int = 30,
        fourcc: str = "MJPG",
        strict_size: bool = True,
    ) -> None:
        self.camera_index = camera_index

        self.requested_width = width
        self.requested_height = height
        self.requested_fps = fps
        self.requested_fourcc = fourcc
        self.strict_size = strict_size

        self.actual_width = 0
        self.actual_height = 0
        self.actual_fps = 0.0

        self._cap: Optional[cv2.VideoCapture] = None
        self._thread: Optional[threading.Thread] = None

        self._running = threading.Event()
        self._lock = threading.Lock()

        self._latest_frame: Optional[np.ndarray] = None
        self._last_error = ""

    @property
    def last_error(self) -> str:
        return self._last_error

    @property
    def actual_size(self) -> tuple[int, int]:
        return self.actual_width, self.actual_height

    def start(self) -> None:
        if self._running.is_set():
            return

        # 项目运行环境是 Linux，优先使用 V4L2。
        cap = cv2.VideoCapture(
            self.camera_index,
            cv2.CAP_V4L2,
        )

        # 部分 OpenCV 构建可能不接受 CAP_V4L2，做一次回退。
        if not cap.isOpened():
            cap.release()
            cap = cv2.VideoCapture(self.camera_index)

        if not cap.isOpened():
            cap.release()
            raise RuntimeError(
                f"无法打开摄像头设备 {self.camera_index}"
            )

        # MJPG 一般应在设置高分辨率之前指定。
        if self.requested_fourcc:
            if len(self.requested_fourcc) != 4:
                cap.release()
                raise ValueError(
                    "摄像头 FOURCC 必须是四个字符"
                )

            cap.set(
                cv2.CAP_PROP_FOURCC,
                cv2.VideoWriter_fourcc(
                    *self.requested_fourcc
                ),
            )

        cap.set(
            cv2.CAP_PROP_FRAME_WIDTH,
            self.requested_width,
        )

        cap.set(
            cv2.CAP_PROP_FRAME_HEIGHT,
            self.requested_height,
        )

        cap.set(
            cv2.CAP_PROP_FPS,
            self.requested_fps,
        )

        # 不是所有驱动都支持，失败也不应终止。
        cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)

        # 先同步读取一帧，确认真正输出尺寸。
        ok, first_frame = cap.read()

        if not ok or first_frame is None:
            cap.release()
            raise RuntimeError("摄像头打开成功，但读取首帧失败")

        actual_height, actual_width = first_frame.shape[:2]

        self.actual_width = actual_width
        self.actual_height = actual_height
        self.actual_fps = cap.get(cv2.CAP_PROP_FPS)

        if self.strict_size and (
            actual_width != self.requested_width
            or actual_height != self.requested_height
        ):
            cap.release()

            raise RuntimeError(
                "摄像头分辨率不匹配：\n"
                f"请求：{self.requested_width}"
                f"×{self.requested_height}\n"
                f"实际：{actual_width}×{actual_height}\n\n"
                "不能直接使用不同分辨率进行 ROI 标定，"
                "否则 GUI 坐标与主程序坐标不一致。"
            )

        with self._lock:
            self._latest_frame = first_frame.copy()

        self._cap = cap
        self._last_error = ""
        self._running.set()

        self._thread = threading.Thread(
            target=self._reader_loop,
            name="calibration-camera",
            daemon=True,
        )

        self._thread.start()

    def _reader_loop(self) -> None:
        assert self._cap is not None

        while self._running.is_set():
            ok, frame = self._cap.read()

            if not ok or frame is None:
                self._last_error = "摄像头读取失败"
                time.sleep(0.03)
                continue

            frame_height, frame_width = frame.shape[:2]

            # 运行过程中尺寸变化时，拒绝使用该帧。
            if (
                frame_width != self.actual_width
                or frame_height != self.actual_height
            ):
                self._last_error = (
                    "摄像头输出尺寸发生变化："
                    f"{frame_width}×{frame_height}"
                )
                continue

            with self._lock:
                self._latest_frame = frame.copy()

    def latest_frame(self) -> Optional[np.ndarray]:
        with self._lock:
            if self._latest_frame is None:
                return None

            return self._latest_frame.copy()

    def stop(self) -> None:
        self._running.clear()

        if self._thread is not None:
            self._thread.join(timeout=1.0)
            self._thread = None

        if self._cap is not None:
            self._cap.release()
            self._cap = None

        with self._lock:
            self._latest_frame = None