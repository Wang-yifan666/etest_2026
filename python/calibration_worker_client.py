"""
etest_2026 标定采样 worker 客户端。

使用持久 subprocess 与 C++ worker 通信，V2 协议带请求 ID。
"""

from __future__ import annotations

import subprocess
import tempfile
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

import numpy as np

try:
    import cv2
    HAS_CV2 = True
except ImportError:
    cv2 = None  # type: ignore
    HAS_CV2 = False

PROJECT_ROOT = Path(__file__).resolve().parent.parent


@dataclass
class WorkerMeasurement:
    """单次推理结果。"""

    request_id: int
    valid: bool

    global_x: float = 0.0
    global_y: float = 0.0
    confidence: float = 0.0

    status: str = "LOST"
    error: str = ""


class CalibrationWorkerClient:
    """持久 worker 子进程客户端（V2 协议）。

    内部使用 subprocess.Popen 启动 etest_calibration_worker，
    通过 stdin/stdout 发送指令和接收结果。
    """

    def __init__(
        self,
        config_dir: Path,
        project_root: Optional[Path] = None,
    ) -> None:
        if project_root is None:
            project_root = PROJECT_ROOT

        self._config_dir = config_dir
        self._worker_binary = (
            project_root / "build" / "etest_calibration_worker"
        )

        self._process: Optional[subprocess.Popen] = None
        self._request_counter = 0
        self._lock = threading.Lock()
        self._pending: dict[int, threading.Event] = {}
        self._results: dict[int, WorkerMeasurement] = {}
        self._ready = threading.Event()
        self._reset_ok_event = threading.Event()
        self._reader_thread: Optional[threading.Thread] = None
        self._stderr_thread: Optional[threading.Thread] = None
        self._stderr_lines: list[str] = []

    # ── 启动 / 停止 ──

    def start(self) -> None:
        """启动 worker 子进程并握手。"""
        if self._process is not None:
            return

        if not self._worker_binary.is_file():
            raise RuntimeError(
                f"找不到 worker 可执行文件：{self._worker_binary}\n"
                "请先执行：cd build && cmake .. && make etest_calibration_worker"
            )

        self._process = subprocess.Popen(
            [
                str(self._worker_binary),
                str(self._config_dir),
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
            cwd=str(PROJECT_ROOT),
        )

        # 发送握手
        try:
            self._process.stdin.write("HELLO\t2\n")  # type: ignore[union-attr]
            self._process.stdin.flush()  # type: ignore[union-attr]
        except OSError as exc:
            self.stop()
            raise RuntimeError(f"向 worker 发送握手失败：{exc}") from exc

        # 启动 stdout/stderr 读取线程
        self._reader_thread = threading.Thread(
            target=self._read_stdout,
            daemon=True,
            name="worker-stdout",
        )
        self._reader_thread.start()

        self._stderr_thread = threading.Thread(
            target=self._read_stderr,
            daemon=True,
            name="worker-stderr",
        )
        self._stderr_thread.start()

        # 等待 READY（超时 10 秒）
        if not self._ready.wait(timeout=10.0):
            self.stop()
            raise RuntimeError(
                "Worker 启动超时，未收到 READY。\n"
                "stderr 最后几行：\n"
                + "\n".join(self._stderr_lines[-5:])
            )

    def stop(self) -> None:
        """优雅关闭 worker。"""
        process = self._process
        if process is None or process.poll() is not None:
            self._process = None
            return

        try:
            process.stdin.write("QUIT\n")  # type: ignore[union-attr]
            process.stdin.flush()  # type: ignore[union-attr]
        except (OSError, BrokenPipeError):
            pass

        try:
            process.wait(timeout=3.0)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=2.0)

        self._process = None

    def restart(self) -> None:
        """停止后重新启动。"""
        self.stop()
        self._pending.clear()
        self._results.clear()
        self._ready.clear()
        self._reset_ok_event.clear()
        self._request_counter = 0
        self.start()

    # ── 命令接口 ──

    def reset_tracking(self) -> None:
        """重置检测器内部轨迹。"""
        if self._process is None or self._process.poll() is not None:
            raise RuntimeError("Worker 未运行")

        self._reset_ok_event.clear()

        try:
            self._process.stdin.write("RESET\n")  # type: ignore[union-attr]
            self._process.stdin.flush()  # type: ignore[union-attr]
        except OSError as exc:
            raise RuntimeError(f"发送 RESET 失败：{exc}") from exc

        if not self._reset_ok_event.wait(timeout=5.0):
            raise RuntimeError("RESET 超时")

    def infer(
        self,
        frame: np.ndarray,
        mode: str = "FULL",
        timeout_s: float = 5.0,
    ) -> WorkerMeasurement:
        """
        对一帧图像进行推理。

        将帧写入 PNG 临时文件，发送 INFER 命令，等待 RESULT。

        参数：
            frame: BGR 图像（numpy 数组）。
            mode: "FULL" 或 "CENTER"。
            timeout_s: 单次推理超时秒数。

        返回：
            WorkerMeasurement，valid=True 表示检测成功。
        """
        if self._process is None or self._process.poll() is not None:
            raise RuntimeError("Worker 未运行")

        if mode.upper() not in ("FULL", "CENTER"):
            raise ValueError(f"未知模式：{mode}，应为 FULL 或 CENTER")

        request_id = self._new_request_id()

        # 写入 PNG 临时文件（低压缩率减少压缩噪声）
        temp_path = (
            Path(tempfile.gettempdir())
            / f"etest_calib_{request_id:06d}.png"
        )

        try:
            if not cv2.imwrite(
                str(temp_path),
                frame,
                [cv2.IMWRITE_PNG_COMPRESSION, 1],
            ):
                return WorkerMeasurement(
                    request_id=request_id,
                    valid=False,
                    status="ERROR",
                    error="写入临时图片失败",
                )

            event = threading.Event()
            with self._lock:
                self._pending[request_id] = event

            try:
                self._process.stdin.write(  # type: ignore[union-attr]
                    f"INFER\t{request_id}\t{temp_path}\t{mode.upper()}\n"
                )
                self._process.stdin.flush()  # type: ignore[union-attr]
            except OSError as exc:
                with self._lock:
                    self._pending.pop(request_id, None)
                return WorkerMeasurement(
                    request_id=request_id,
                    valid=False,
                    status="ERROR",
                    error=f"发送推理命令失败：{exc}",
                )

            if not event.wait(timeout=timeout_s):
                # 超时：不清除 request_id 映射，
                # 防止后续旧消息污染新请求
                return WorkerMeasurement(
                    request_id=request_id,
                    valid=False,
                    status="TIMEOUT",
                )

            with self._lock:
                result = self._results.pop(
                    request_id,
                    WorkerMeasurement(
                        request_id=request_id,
                        valid=False,
                        status="LOST",
                    ),
                )
                return result

        finally:
            # 清理临时文件
            try:
                temp_path.unlink(missing_ok=True)
            except OSError:
                pass

    # ── 内部 ──

    def _new_request_id(self) -> int:
        with self._lock:
            self._request_counter += 1
            return self._request_counter

    def _read_stdout(self) -> None:
        """后台线程：持续读取 worker stdout。"""
        if self._process is None:
            return

        try:
            for line in self._process.stdout:  # type: ignore[union-attr]
                line = line.strip()
                if not line:
                    continue

                self._handle_stdout_line(line)
        except (OSError, ValueError):
            pass

    def _handle_stdout_line(self, line: str) -> None:
        """解析单行 stdout 消息。"""
        if line == "READY\t2":
            self._ready.set()
            return

        if line == "RESET_OK":
            self._reset_ok_event.set()
            return

        if line.startswith("RESULT\t"):
            parts = line.split("\t")

            if len(parts) < 3:
                return

            try:
                request_id = int(parts[1])
            except ValueError:
                return

            status = parts[2]
            measurement = WorkerMeasurement(
                request_id=request_id,
                valid=False,
            )

            if status == "OK" and len(parts) >= 6:
                try:
                    measurement.valid = True
                    measurement.status = "OK"
                    measurement.global_x = float(parts[3])
                    measurement.global_y = float(parts[4])
                    measurement.confidence = float(parts[5])
                except ValueError:
                    measurement.status = "ERROR"
                    measurement.error = "解析坐标/置信度失败"
            elif status == "LOST":
                measurement.status = "LOST"
            elif status == "ERROR":
                measurement.status = "ERROR"
                measurement.error = (
                    parts[3] if len(parts) >= 4 else "未知错误"
                )
            else:
                return  # 未知状态，忽略

            with self._lock:
                self._results[request_id] = measurement
                event = self._pending.get(request_id)
                if event is not None:
                    event.set()

    def _read_stderr(self) -> None:
        """后台线程：读取 worker stderr 日志。"""
        if self._process is None:
            return

        try:
            for line in self._process.stderr:  # type: ignore[union-attr]
                if line:
                    self._stderr_lines.append(line.rstrip())
                    # 保留最近 200 行
                    if len(self._stderr_lines) > 200:
                        self._stderr_lines = self._stderr_lines[-200:]
        except (OSError, ValueError):
            pass

    def stderr_tail(self, lines: int = 20) -> str:
        """返回 worker stderr 尾部日志。"""
        return "\n".join(self._stderr_lines[-lines:])