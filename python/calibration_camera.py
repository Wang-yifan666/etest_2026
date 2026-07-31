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
    def __init__(self, camera_index: int = 0) -> None:
        self.camera_index = camera_index
        self._cap: Optional[cv2.VideoCapture] = None
        self._thread: Optional[threading.Thread] = None
        self._running = threading.Event()
        self._lock = threading.Lock()
        self._latest_frame: Optional[np.ndarray] = None
        self._last_error = ""

    @property
    def last_error(self) -> str:
        return self._last_error

    def start(self) -> None:
        if self._running.is_set():
            return

        cap = cv2.VideoCapture(self.camera_index)

        if not cap.isOpened():
            cap.release()
            raise RuntimeError(
                f"无法打开摄像头设备 {self.camera_index}"
            )

        self._cap = cap
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