#!/usr/bin/env python3
"""
etest_2026 简易控制面板
运行：
    python3 python/gui.py
"""

from __future__ import annotations

import functools
import logging
from logging.handlers import RotatingFileHandler
import os
from pathlib import Path
import queue
import shutil
import signal
import subprocess
import tempfile
import threading
import time
import tkinter as tk
from tkinter import messagebox, ttk
import json
import re
from typing import Callable, Optional, TypeVar 

try:
    import tomllib  # Python 3.11+
except ImportError:
    try:
        import tomli as tomllib  # type: ignore[no-redef]
    except ImportError:
        tomllib = None  # type: ignore[assignment]

try:
    import tomlkit
except ImportError:
    tomlkit = None

from calibration_model import (  # type: ignore[import-untyped]
    CalibrationValidationError,
    CanvasTransform,
    DetectorRegion,
    RoiCalibration,
    RoiHandle,
    RoiRect,
    AxisCalibration,
    AxisCalibrationPoint,
    clamp,
    robust_sample,
)
from calibration_camera import CalibrationCamera  # type: ignore[import-untyped]
from calibration_worker_client import (  # type: ignore[import-untyped]
    CalibrationWorkerClient,
    WorkerMeasurement,
)

try:
    import cv2
    import numpy as np
    HAS_CV2 = True
except ImportError:
    cv2 = None  # type: ignore
    np = None  # type: ignore
    HAS_CV2 = False


# 路径与常量

SCRIPT_PATH = Path(__file__).resolve()
PROJECT_ROOT = SCRIPT_PATH.parent.parent
CONFIG_DIR = PROJECT_ROOT / "config"
EXECUTABLE = PROJECT_ROOT / "build" / "etest_2026"
LOG_DIR = PROJECT_ROOT / "data" / "log"
GUI_LOG_DIR = LOG_DIR / "gui"
RUN_DIR = PROJECT_ROOT / "data" / "run"

GUI_LOG_FILE = GUI_LOG_DIR / "gui.log"
GUI_PREFS_FILE = GUI_LOG_DIR / "gui_prefs.json"
PROCESS_CONSOLE_LOG = GUI_LOG_DIR / "process_console.log"
PID_FILE = RUN_DIR / "etest_2026.pid"

SERVICE_NAME = "etest_2026"
MODE_DIRECT = "直接进程"
MODE_SYSTEMD = "systemd 服务"

STATUS_INTERVAL_MS = 1500
LOG_INTERVAL_MS = 500
MAX_LOG_LINES = 10000

T = TypeVar("T")


# 日志

def setup_gui_logger() -> logging.Logger:
    GUI_LOG_DIR.mkdir(parents=True, exist_ok=True)

    logger = logging.getLogger("etest_gui")
    logger.setLevel(logging.DEBUG)
    logger.propagate = False

    if logger.handlers:
        return logger

    handler = RotatingFileHandler(
        GUI_LOG_FILE,
        maxBytes=2 * 1024 * 1024,
        backupCount=3,
        encoding="utf-8",
    )
    handler.setFormatter(
        logging.Formatter(
            "%(asctime)s - %(levelname)s - GUI - %(message)s",
            datefmt="%Y-%m-%d %H:%M:%S",
        )
    )
    logger.addHandler(handler)
    return logger


LOGGER = setup_gui_logger()


# 通用辅助

class OperationError(RuntimeError):
    """可向用户展示的操作失败。"""


def ui_guard(action_name: str) -> Callable[[Callable[..., T]], Callable[..., Optional[T]]]:
    """捕获 Tk 回调中的异常，确保单次错误不会让 GUI 退出。"""

    def decorator(func: Callable[..., T]) -> Callable[..., Optional[T]]:
        @functools.wraps(func)
        def wrapper(self: "EtestGui", *args, **kwargs) -> Optional[T]:
            try:
                return func(self, *args, **kwargs)
            except Exception as exc:
                LOGGER.exception("%s失败", action_name)
                self.set_notice(f"{action_name}失败：{exc}", error=True)
                messagebox.showerror(action_name, str(exc), parent=self.root)
                return None

        return wrapper

    return decorator


def atomic_write_text(path: Path, text: str, make_backup: bool = True) -> None:
    """
    在同一目录中写临时文件，再使用 os.replace 原子替换。
    成功替换前，原文件不会被破坏。
    """

    path.parent.mkdir(parents=True, exist_ok=True)
    temp_path: Optional[Path] = None

    try:
        if make_backup and path.exists():
            backup_path = path.with_suffix(path.suffix + ".bak")
            shutil.copy2(path, backup_path)

        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            newline="\n",
            dir=path.parent,
            prefix=f".{path.name}.",
            suffix=".tmp",
            delete=False,
        ) as temp_file:
            temp_path = Path(temp_file.name)
            temp_file.write(text)
            temp_file.flush()
            os.fsync(temp_file.fileno())

        os.replace(temp_path, path)
        temp_path = None

        # 尽量把目录项也刷入磁盘；不支持时不影响保存结果。
        if hasattr(os, "O_DIRECTORY"):
            try:
                directory_fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
                try:
                    os.fsync(directory_fd)
                finally:
                    os.close(directory_fd)
            except OSError:
                LOGGER.warning("无法 fsync 配置目录：%s", path.parent, exc_info=True)

    finally:
        if temp_path is not None:
            try:
                temp_path.unlink(missing_ok=True)
            except OSError:
                LOGGER.warning("无法删除临时文件：%s", temp_path, exc_info=True)


def run_command(
    command: list[str],
    *,
    timeout: float = 5.0,
    cwd: Optional[Path] = None,
) -> subprocess.CompletedProcess[str]:
    LOGGER.debug("执行命令：%s", command)
    return subprocess.run(
        command,
        cwd=str(cwd) if cwd else None,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=timeout,
        check=False,
    )


def tail_text_file(path: Path, max_lines: int = 40) -> str:
    """读取文本文件尾部。文件较小时实现简单可靠。"""

    try:
        with path.open("r", encoding="utf-8", errors="replace") as file:
            lines = file.readlines()
        return "".join(lines[-max_lines:]).strip()
    except OSError:
        LOGGER.warning("读取文件尾部失败：%s", path, exc_info=True)
        return ""


# 主界面

class EtestGui:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("etest_2026")
        self.root.geometry("1080x740")
        self.root.minsize(900, 600)
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)
        self.root.report_callback_exception = self.on_tk_exception

        self.ui_queue: queue.Queue[tuple[Callable, tuple, dict]] = queue.Queue()
        self.operation_busy = False
        self.status_query_running = False
        self.closed = False

        self.process: Optional[subprocess.Popen] = None
        self.process_console_handle = None

        self.current_log_path: Optional[Path] = None
        self.current_log_offset = 0

        self.current_config_rel: Optional[str] = None
        self.config_dirty = False
        self.loading_config = False

        default_mode = (
            MODE_SYSTEMD if self.systemd_service_installed() else MODE_DIRECT
        )

        self.control_mode_var = tk.StringVar(value=default_mode)
        self.status_var = tk.StringVar(value="正在检查")
        self.pid_var = tk.StringVar(value="-")
        self.notice_var = tk.StringVar(value="就绪")
        self.current_log_var = tk.StringVar(value="尚未发现主程序日志")
        self.auto_log_var = tk.BooleanVar(value=True)
        self.follow_log_var = tk.BooleanVar(value=True)
        self.errors_only_var = tk.BooleanVar(value=False)
        self.config_file_var = tk.StringVar()
        self.mode_hint_var = tk.StringVar(value="当前配置模式：未知")

        self.font_size_var = tk.IntVar(value=10)
        self._load_gui_prefs()

        self.build_ui()
        self.refresh_config_file_list()
        self.load_initial_config()

        self.root.after(100, self.drain_ui_queue)
        self.root.after(200, self.periodic_status_refresh)
        self.root.after(300, self.periodic_log_refresh)

        LOGGER.info(
            "GUI 启动，project_root=%s, control_mode=%s",
            PROJECT_ROOT,
            default_mode,
        )

    # UI 构建
    
    def build_ui(self) -> None:
        outer = ttk.Frame(self.root, padding=10)
        outer.pack(fill=tk.BOTH, expand=True)

        control = ttk.LabelFrame(outer, text="程序控制", padding=10)
        control.pack(fill=tk.X)

        ttk.Label(control, text="运行方式：").grid(
            row=0, column=0, sticky=tk.W, padx=(0, 4)
        )
        mode_box = ttk.Combobox(
            control,
            textvariable=self.control_mode_var,
            values=(MODE_DIRECT, MODE_SYSTEMD),
            state="readonly",
            width=14,
        )
        mode_box.grid(row=0, column=1, sticky=tk.W)
        mode_box.bind("<<ComboboxSelected>>", self.on_control_mode_changed)

        ttk.Label(control, text="状态：").grid(
            row=0, column=2, sticky=tk.E, padx=(20, 4)
        )
        self.status_label = tk.Label(
            control,
            textvariable=self.status_var,
            width=12,
            relief=tk.GROOVE,
            padx=6,
        )
        self.status_label.grid(row=0, column=3, sticky=tk.W)

        ttk.Label(control, text="PID：").grid(
            row=0, column=4, sticky=tk.E, padx=(20, 4)
        )
        ttk.Label(control, textvariable=self.pid_var, width=10).grid(
            row=0, column=5, sticky=tk.W
        )

        button_frame = ttk.Frame(control)
        button_frame.grid(row=0, column=6, sticky=tk.E, padx=(20, 0))

        self.start_button = ttk.Button(
            button_frame, text="启动", command=self.on_start, width=9
        )
        self.start_button.pack(side=tk.LEFT, padx=3)

        self.stop_button = ttk.Button(
            button_frame, text="停止", command=self.on_stop, width=9
        )
        self.stop_button.pack(side=tk.LEFT, padx=3)

        self.restart_button = ttk.Button(
            button_frame, text="重启", command=self.on_restart, width=9
        )
        self.restart_button.pack(side=tk.LEFT, padx=3)

        self.refresh_button = ttk.Button(
            button_frame, text="刷新状态", command=self.on_refresh_status, width=10
        )
        self.refresh_button.pack(side=tk.LEFT, padx=3)

        control.columnconfigure(6, weight=1)

        path_text = (
            f"项目：{PROJECT_ROOT}    "
            f"程序：{EXECUTABLE.relative_to(PROJECT_ROOT)}    "
            f"配置：{CONFIG_DIR.relative_to(PROJECT_ROOT)}"
        )
        ttk.Label(control, text=path_text).grid(
            row=1, column=0, columnspan=7, sticky=tk.W, pady=(8, 0)
        )

        self.notebook = ttk.Notebook(outer)
        self.notebook.pack(fill=tk.BOTH, expand=True, pady=(10, 0))

        self.log_tab = ttk.Frame(self.notebook, padding=8)
        self.config_tab = ttk.Frame(self.notebook, padding=8)
        self.calib_tab = ttk.Frame(self.notebook, padding=8)
        self.notebook.add(self.log_tab, text="日志")
        self.notebook.add(self.config_tab, text="配置")
        self.notebook.add(self.calib_tab, text="标定")

        self.build_log_tab()
        self.build_config_tab()
        self.build_calibration_tab()

        # 应用从偏好加载的字号
        self._on_font_size_changed()

        notice = ttk.Label(
            outer,
            textvariable=self.notice_var,
            anchor=tk.W,
            relief=tk.SUNKEN,
            padding=(6, 3),
        )
        notice.pack(fill=tk.X, pady=(8, 0))

    def build_log_tab(self) -> None:
        toolbar = ttk.Frame(self.log_tab)
        toolbar.pack(fill=tk.X)

        ttk.Checkbutton(
            toolbar,
            text="自动刷新",
            variable=self.auto_log_var,
        ).pack(side=tk.LEFT, padx=(0, 8))

        ttk.Checkbutton(
            toolbar,
            text="自动滚动",
            variable=self.follow_log_var,
        ).pack(side=tk.LEFT, padx=(0, 8))

        ttk.Checkbutton(
            toolbar,
            text="仅 ERROR/FATAL",
            variable=self.errors_only_var,
            command=self.on_log_filter_changed,
        ).pack(side=tk.LEFT, padx=(0, 8))

        ttk.Button(
            toolbar, text="重新加载", command=self.on_reload_log
        ).pack(side=tk.LEFT, padx=3)

        ttk.Button(
            toolbar, text="清空显示", command=self.on_clear_log_view
        ).pack(side=tk.LEFT, padx=3)

        ttk.Button(
            toolbar, text="打开日志目录", command=self.on_open_log_dir
        ).pack(side=tk.LEFT, padx=3)

        ttk.Label(toolbar, text="  字号：").pack(side=tk.LEFT)
        self.font_spinbox = ttk.Spinbox(
            toolbar,
            from_=8,
            to=24,
            width=4,
            textvariable=self.font_size_var,
            command=self._on_font_size_changed,
        )
        self.font_spinbox.pack(side=tk.LEFT)

        ttk.Label(
            self.log_tab,
            textvariable=self.current_log_var,
            anchor=tk.W,
        ).pack(fill=tk.X, pady=(8, 5))

        text_frame = ttk.Frame(self.log_tab)
        text_frame.pack(fill=tk.BOTH, expand=True)

        self.log_text = tk.Text(
            text_frame,
            wrap=tk.NONE,
            state=tk.DISABLED,
            font=("TkFixedFont", 10),
        )
        log_y = ttk.Scrollbar(
            text_frame, orient=tk.VERTICAL, command=self.log_text.yview
        )
        log_x = ttk.Scrollbar(
            text_frame, orient=tk.HORIZONTAL, command=self.log_text.xview
        )
        self.log_text.configure(
            yscrollcommand=log_y.set,
            xscrollcommand=log_x.set,
        )

        self.log_text.grid(row=0, column=0, sticky="nsew")
        log_y.grid(row=0, column=1, sticky="ns")
        log_x.grid(row=1, column=0, sticky="ew")
        text_frame.rowconfigure(0, weight=1)
        text_frame.columnconfigure(0, weight=1)

        self.log_text.tag_configure("ERROR", foreground="#b00020")
        self.log_text.tag_configure("FATAL", foreground="#7f0000")
        self.log_text.tag_configure("WARN", foreground="#8a5a00")

    def build_config_tab(self) -> None:
        toolbar = ttk.Frame(self.config_tab)
        toolbar.pack(fill=tk.X)

        ttk.Label(toolbar, text="配置文件：").pack(side=tk.LEFT)

        self.config_box = ttk.Combobox(
            toolbar,
            textvariable=self.config_file_var,
            state="readonly",
            width=32,
        )
        self.config_box.pack(side=tk.LEFT, padx=(4, 8))
        self.config_box.bind("<<ComboboxSelected>>", self.on_config_selected)

        ttk.Button(
            toolbar, text="重新加载", command=self.on_reload_config
        ).pack(side=tk.LEFT, padx=3)

        ttk.Button(
            toolbar, text="校验 TOML", command=self.on_validate_config
        ).pack(side=tk.LEFT, padx=3)

        ttk.Button(
            toolbar, text="保存", command=self.on_save_config
        ).pack(side=tk.LEFT, padx=3)

        ttk.Button(
            toolbar, text="保存并重启", command=self.on_save_and_restart
        ).pack(side=tk.LEFT, padx=3)

        ttk.Label(
            self.config_tab,
            textvariable=self.mode_hint_var,
            anchor=tk.W,
        ).pack(fill=tk.X, pady=(8, 5))

        tip = (
            "配置优先级提示：modes/<当前模式>.toml 会覆盖基础配置。"
            "保存时会先校验 TOML，并生成同名 .bak 备份。"
        )
        ttk.Label(self.config_tab, text=tip, anchor=tk.W).pack(fill=tk.X)

        editor_frame = ttk.Frame(self.config_tab)
        editor_frame.pack(fill=tk.BOTH, expand=True, pady=(6, 0))

        self.config_text = tk.Text(
            editor_frame,
            wrap=tk.NONE,
            undo=True,
            font=("TkFixedFont", 11),
        )
        config_y = ttk.Scrollbar(
            editor_frame, orient=tk.VERTICAL, command=self.config_text.yview
        )
        config_x = ttk.Scrollbar(
            editor_frame, orient=tk.HORIZONTAL, command=self.config_text.xview
        )
        self.config_text.configure(
            yscrollcommand=config_y.set,
            xscrollcommand=config_x.set,
        )
        self.config_text.bind("<<Modified>>", self.on_config_modified)

        self.config_text.grid(row=0, column=0, sticky="nsew")
        config_y.grid(row=0, column=1, sticky="ns")
        config_x.grid(row=1, column=0, sticky="ew")
        editor_frame.rowconfigure(0, weight=1)
        editor_frame.columnconfigure(0, weight=1)

    # 线程与异常

    def post_ui(self, callback: Callable, *args, **kwargs) -> None:
        self.ui_queue.put((callback, args, kwargs))

    def drain_ui_queue(self) -> None:
        if self.closed:
            return

        try:
            while True:
                callback, args, kwargs = self.ui_queue.get_nowait()
                try:
                    callback(*args, **kwargs)
                except Exception:
                    LOGGER.exception("处理后台线程返回结果失败")
        except queue.Empty:
            pass

        self.root.after(100, self.drain_ui_queue)

    def run_background(
        self,
        action_name: str,
        worker: Callable[[], str],
        on_success: Optional[Callable[[str], None]] = None,
    ) -> None:
        if self.operation_busy:
            self.set_notice("已有操作正在执行，请稍后", error=True)
            return

        self.operation_busy = True
        self.set_control_buttons_enabled(False)
        self.set_notice(f"{action_name}中……")

        def task() -> None:
            try:
                message = worker()
            except Exception as exc:
                LOGGER.exception("%s失败", action_name)
                self.post_ui(self.finish_background_error, action_name, str(exc))
            else:
                self.post_ui(
                    self.finish_background_success,
                    action_name,
                    message,
                    on_success,
                )

        threading.Thread(
            target=task,
            name=f"gui-{action_name}",
            daemon=True,
        ).start()

    def finish_background_success(
        self,
        action_name: str,
        message: str,
        on_success: Optional[Callable[[str], None]],
    ) -> None:
        self.operation_busy = False
        self.set_control_buttons_enabled(True)
        self.set_notice(message)
        LOGGER.info("%s成功：%s", action_name, message)

        if on_success:
            on_success(message)

        self.request_status_refresh()

    def finish_background_error(self, action_name: str, message: str) -> None:
        self.operation_busy = False
        self.set_control_buttons_enabled(True)
        self.set_notice(f"{action_name}失败：{message}", error=True)
        messagebox.showerror(action_name, message, parent=self.root)
        self.request_status_refresh()

    def on_tk_exception(self, exc_type, exc_value, exc_traceback) -> None:
        LOGGER.error(
            "Tk 未捕获异常",
            exc_info=(exc_type, exc_value, exc_traceback),
        )
        self.set_notice(f"界面操作异常：{exc_value}", error=True)
        messagebox.showerror(
            "界面异常",
            f"{exc_type.__name__}: {exc_value}",
            parent=self.root,
        )

    # 程序控制：公共入口

    @ui_guard("启动程序")
    def on_start(self) -> None:
        mode = self.control_mode_var.get()
        self.run_background(
            "启动程序",
            lambda: self.start_backend(mode),
        )

    @ui_guard("停止程序")
    def on_stop(self) -> None:
        mode = self.control_mode_var.get()
        self.run_background(
            "停止程序",
            lambda: self.stop_backend(mode),
        )

    @ui_guard("重启程序")
    def on_restart(self) -> None:
        mode = self.control_mode_var.get()
        self.run_background(
            "重启程序",
            lambda: self.restart_backend(mode),
        )

    @ui_guard("刷新状态")
    def on_refresh_status(self) -> None:
        self.request_status_refresh(force=True)

    @ui_guard("切换运行方式")
    def on_control_mode_changed(self, _event=None) -> None:
        mode = self.control_mode_var.get()
        LOGGER.info("运行方式切换为：%s", mode)
        self.set_notice(f"运行方式已切换为：{mode}")
        self.request_status_refresh(force=True)

    def start_backend(self, mode: str) -> str:
        if mode == MODE_SYSTEMD:
            return self.start_systemd()
        return self.start_direct()

    def stop_backend(self, mode: str) -> str:
        if mode == MODE_SYSTEMD:
            return self.stop_systemd()
        return self.stop_direct()

    def restart_backend(self, mode: str) -> str:
        if mode == MODE_SYSTEMD:
            return self.restart_systemd()

        try:
            self.stop_direct()
        except OperationError as exc:
            # “未运行”不妨碍继续启动；其他停止错误必须暴露。
            if "未运行" not in str(exc):
                raise

        time.sleep(0.2)
        return self.start_direct()

    # 程序控制：直接进程

    def validate_launch_paths(self) -> None:
        if not EXECUTABLE.is_file():
            raise OperationError(
                f"找不到可执行文件：{EXECUTABLE}\n"
                "请先执行：bash scripts/build.sh"
            )

        if not os.access(EXECUTABLE, os.X_OK):
            raise OperationError(f"文件不可执行：{EXECUTABLE}")

        if not CONFIG_DIR.is_dir():
            raise OperationError(f"找不到配置目录：{CONFIG_DIR}")

    def start_direct(self) -> str:
        self.validate_launch_paths()

        running_pid = self.get_direct_pid()
        if running_pid is not None:
            return f"程序已经运行，PID={running_pid}"

        GUI_LOG_DIR.mkdir(parents=True, exist_ok=True)
        RUN_DIR.mkdir(parents=True, exist_ok=True)

        console_handle = PROCESS_CONSOLE_LOG.open(
            "a",
            encoding="utf-8",
            buffering=1,
        )
        console_handle.write(
            f"\n[{time.strftime('%Y-%m-%d %H:%M:%S')}] "
            "GUI launching etest_2026\n"
        )
        console_handle.flush()

        try:
            process = subprocess.Popen(
                [
                    str(EXECUTABLE),
                    "--config-dir",
                    str(CONFIG_DIR),
                ],
                cwd=str(PROJECT_ROOT),
                stdin=subprocess.DEVNULL,
                stdout=console_handle,
                stderr=subprocess.STDOUT,
                start_new_session=True,
                close_fds=True,
            )
        except Exception:
            console_handle.close()
            raise

        # 尽早识别配置、动态库等初始化失败。
        time.sleep(0.5)
        exit_code = process.poll()
        if exit_code is not None:
            console_handle.close()
            details = tail_text_file(PROCESS_CONSOLE_LOG, 30)
            raise OperationError(
                f"程序启动后立即退出，返回码={exit_code}\n"
                f"{details or '请查看 GUI 的 process_console.log'}"
            )

        self.process = process
        self.process_console_handle = console_handle
        atomic_write_text(PID_FILE, f"{process.pid}\n", make_backup=False)

        LOGGER.info("直接进程启动成功，PID=%s", process.pid)
        return f"程序已启动，PID={process.pid}"

    def stop_direct(self) -> str:
        pid = self.get_direct_pid()
        if pid is None:
            raise OperationError("程序未运行")

        LOGGER.info("向进程组发送 SIGTERM，PID=%s", pid)

        try:
            os.killpg(pid, signal.SIGTERM)
        except ProcessLookupError:
            self.remove_pid_file()
            return "进程已经结束"
        except PermissionError as exc:
            raise OperationError(f"无权停止 PID={pid}：{exc}") from exc
        except OSError:
            LOGGER.warning(
                "向进程组发送 SIGTERM 失败，尝试发送给单进程",
                exc_info=True,
            )
            try:
                os.kill(pid, signal.SIGTERM)
            except ProcessLookupError:
                self.remove_pid_file()
                return "进程已经结束"

        deadline = time.monotonic() + 6.0
        while time.monotonic() < deadline:
            if not self.pid_alive(pid):
                self.cleanup_direct_process()
                return f"程序已停止，PID={pid}"
            time.sleep(0.1)

        LOGGER.error("SIGTERM 超时，发送 SIGKILL，PID=%s", pid)
        try:
            os.killpg(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        except OSError:
            LOGGER.warning("向进程组发送 SIGKILL 失败", exc_info=True)
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass

        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline:
            if not self.pid_alive(pid):
                self.cleanup_direct_process()
                return f"程序已强制停止，PID={pid}"
            time.sleep(0.1)

        raise OperationError(f"无法停止进程 PID={pid}")

    def cleanup_direct_process(self) -> None:
        self.remove_pid_file()

        if self.process_console_handle is not None:
            try:
                self.process_console_handle.close()
            except OSError:
                LOGGER.warning("关闭进程控制台日志失败", exc_info=True)
            self.process_console_handle = None

        self.process = None

    def get_direct_pid(self) -> Optional[int]:
        if self.process is not None and self.process.poll() is None:
            return self.process.pid

        pid = self.read_pid_file()
        if pid is None:
            return None

        if self.pid_alive(pid) and self.pid_matches_program(pid):
            return pid

        LOGGER.warning("发现失效或不匹配的 PID 文件：%s", pid)
        self.remove_pid_file()
        return None

    def read_pid_file(self) -> Optional[int]:
        try:
            text = PID_FILE.read_text(encoding="utf-8").strip()
            pid = int(text)
            return pid if pid > 0 else None
        except FileNotFoundError:
            return None
        except (OSError, ValueError):
            LOGGER.exception("读取 PID 文件失败：%s", PID_FILE)
            return None

    def remove_pid_file(self) -> None:
        try:
            PID_FILE.unlink(missing_ok=True)
        except OSError:
            LOGGER.warning("删除 PID 文件失败：%s", PID_FILE, exc_info=True)

    @staticmethod
    def pid_alive(pid: int) -> bool:
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return False
        except PermissionError:
            return True
        except OSError:
            return False

        stat_path = Path(f"/proc/{pid}/stat")
        try:
            fields = stat_path.read_text(encoding="utf-8", errors="replace").split()
            if len(fields) >= 3 and fields[2] == "Z":
                return False
        except OSError:
            pass

        return True

    @staticmethod
    def pid_matches_program(pid: int) -> bool:
        cmdline_path = Path(f"/proc/{pid}/cmdline")
        try:
            raw = cmdline_path.read_bytes()
            command = raw.replace(b"\0", b" ").decode(
                "utf-8", errors="replace"
            )
        except OSError:
            LOGGER.warning("无法读取 PID=%s 的 cmdline", pid, exc_info=True)
            return False

        return EXECUTABLE.name in command and "--config-dir" in command

    # 程序控制：systemd

    @staticmethod
    def systemd_service_installed() -> bool:
        if shutil.which("systemctl") is None:
            return False

        candidates = (
            Path(f"/etc/systemd/system/{SERVICE_NAME}.service"),
            Path(f"/lib/systemd/system/{SERVICE_NAME}.service"),
            Path(f"/usr/lib/systemd/system/{SERVICE_NAME}.service"),
        )
        return any(path.exists() for path in candidates)

    def systemctl_change(self, action: str) -> str:
        if shutil.which("systemctl") is None:
            raise OperationError("系统中没有 systemctl")

        command = ["systemctl", action, SERVICE_NAME]
        result = run_command(command, timeout=12.0)

        if result.returncode == 0:
            return result.stdout.strip()

        output_lower = result.stdout.lower()
        permission_problem = any(
            token in output_lower
            for token in (
                "authentication",
                "interactive authentication",
                "access denied",
                "permission denied",
                "not authorized",
            )
        )

        if permission_problem and os.geteuid() != 0 and shutil.which("sudo"):
            sudo_result = run_command(
                ["sudo", "-n", "systemctl", action, SERVICE_NAME],
                timeout=12.0,
            )
            if sudo_result.returncode == 0:
                return sudo_result.stdout.strip()

            raise OperationError(
                f"systemctl {action} 失败：\n"
                f"{sudo_result.stdout.strip() or 'sudo 需要密码或权限不足'}\n\n"
                "比赛机上可为该服务配置免密 systemctl，"
                "或从终端使用 sudo 启动 GUI。"
            )

        raise OperationError(
            f"systemctl {action} 失败：\n"
            f"{result.stdout.strip() or f'返回码={result.returncode}'}"
        )

    def start_systemd(self) -> str:
        self.systemctl_change("start")
        active, pid, detail = self.get_systemd_status()
        if not active:
            raise OperationError(f"服务启动命令已执行，但状态不是 active：{detail}")
        return f"systemd 服务已启动，PID={pid or '-'}"

    def stop_systemd(self) -> str:
        self.systemctl_change("stop")
        active, _, detail = self.get_systemd_status()
        if active:
            raise OperationError(f"服务停止命令已执行，但仍为 active：{detail}")
        return "systemd 服务已停止"

    def restart_systemd(self) -> str:
        self.systemctl_change("restart")
        active, pid, detail = self.get_systemd_status()
        if not active:
            raise OperationError(f"服务重启后状态不是 active：{detail}")
        return f"systemd 服务已重启，PID={pid or '-'}"

    def get_systemd_status(self) -> tuple[bool, Optional[int], str]:
        if shutil.which("systemctl") is None:
            return False, None, "未安装 systemctl"

        active_result = run_command(
            ["systemctl", "is-active", SERVICE_NAME],
            timeout=3.0,
        )
        status_text = active_result.stdout.strip() or "unknown"
        active = active_result.returncode == 0 and status_text == "active"

        pid: Optional[int] = None
        if active:
            pid_result = run_command(
                [
                    "systemctl",
                    "show",
                    SERVICE_NAME,
                    "--property=MainPID",
                    "--value",
                ],
                timeout=3.0,
            )
            try:
                parsed_pid = int(pid_result.stdout.strip())
                if parsed_pid > 0:
                    pid = parsed_pid
            except ValueError:
                LOGGER.warning(
                    "无法解析 systemd MainPID：%r",
                    pid_result.stdout,
                )

        return active, pid, status_text

    # 状态刷新

    def periodic_status_refresh(self) -> None:
        if self.closed:
            return

        self.request_status_refresh()
        self.root.after(STATUS_INTERVAL_MS, self.periodic_status_refresh)

    def request_status_refresh(self, force: bool = False) -> None:
        if self.status_query_running:
            return

        if self.operation_busy and not force:
            return

        mode = self.control_mode_var.get()
        self.status_query_running = True

        def worker() -> None:
            try:
                if mode == MODE_SYSTEMD:
                    active, pid, detail = self.get_systemd_status()
                else:
                    pid = self.get_direct_pid()
                    active = pid is not None
                    detail = "running" if active else "stopped"
            except Exception as exc:
                LOGGER.exception("刷新程序状态失败")
                self.post_ui(
                    self.apply_status_error,
                    mode,
                    str(exc),
                )
            else:
                self.post_ui(
                    self.apply_status,
                    mode,
                    active,
                    pid,
                    detail,
                )

        threading.Thread(
            target=worker,
            name="gui-status",
            daemon=True,
        ).start()

    def apply_status(
        self,
        queried_mode: str,
        active: bool,
        pid: Optional[int],
        detail: str,
    ) -> None:
        self.status_query_running = False

        if queried_mode != self.control_mode_var.get():
            return

        if active:
            self.status_var.set("运行中")
            self.status_label.configure(bg="#c8e6c9", fg="#1b5e20")
            self.pid_var.set(str(pid or "-"))
        else:
            self.status_var.set("已停止")
            self.status_label.configure(bg="#eeeeee", fg="#424242")
            self.pid_var.set("-")

        self.status_label.update_idletasks()

        if detail not in ("running", "stopped", "active", "inactive"):
            self.set_notice(f"状态：{detail}")

    def apply_status_error(self, queried_mode: str, message: str) -> None:
        self.status_query_running = False

        if queried_mode != self.control_mode_var.get():
            return

        self.status_var.set("状态异常")
        self.status_label.configure(bg="#ffcdd2", fg="#b71c1c")
        self.pid_var.set("-")
        self.set_notice(f"状态检查失败：{message}", error=True)

    # 日志查看

    def periodic_log_refresh(self) -> None:
        if self.closed:
            return

        if self.auto_log_var.get():
            try:
                self.read_log_incremental()
            except Exception:
                LOGGER.exception("自动刷新日志失败")
                self.set_notice("自动刷新日志失败，请查看 GUI 日志", error=True)

        self.root.after(LOG_INTERVAL_MS, self.periodic_log_refresh)

    def find_latest_program_log(self) -> Optional[Path]:
        if not LOG_DIR.is_dir():
            return None

        candidates = []
        try:
            for path in LOG_DIR.glob("*.log"):
                # C++ 日志名以日期数字开头，排除 GUI 自身日志。
                if path.is_file() and path.name[:1].isdigit():
                    candidates.append(path)
        except OSError:
            LOGGER.exception("扫描日志目录失败：%s", LOG_DIR)
            return None

        if not candidates:
            return None

        try:
            return max(candidates, key=lambda item: item.stat().st_mtime_ns)
        except OSError:
            LOGGER.exception("读取日志文件时间失败")
            return None

    def read_log_incremental(self, force_reset: bool = False) -> None:
        latest = self.find_latest_program_log()

        if latest is None:
            self.current_log_var.set(f"未发现日志：{LOG_DIR}")
            return

        if force_reset or latest != self.current_log_path:
            self.current_log_path = latest
            self.current_log_offset = 0
            self.clear_log_text()
            self.current_log_var.set(f"当前日志：{latest.name}")

        try:
            file_size = latest.stat().st_size
            if file_size < self.current_log_offset:
                self.current_log_offset = 0
                self.clear_log_text()

            with latest.open("r", encoding="utf-8", errors="replace") as file:
                file.seek(self.current_log_offset)
                new_text = file.read()
                self.current_log_offset = file.tell()
        except OSError:
            LOGGER.exception("读取日志失败：%s", latest)
            self.set_notice(f"读取日志失败：{latest.name}", error=True)
            return

        if not new_text:
            return

        lines = new_text.splitlines(keepends=True)
        if self.errors_only_var.get():
            lines = [
                line
                for line in lines
                if " - ERROR - " in line or " - FATAL - " in line
            ]

        if not lines:
            return

        self.log_text.configure(state=tk.NORMAL)
        for line in lines:
            tag = ""
            if " - FATAL - " in line:
                tag = "FATAL"
            elif " - ERROR - " in line:
                tag = "ERROR"
            elif " - WARN - " in line:
                tag = "WARN"

            self.log_text.insert(tk.END, line, tag)

        self.trim_log_text()
        self.log_text.configure(state=tk.DISABLED)

        if self.follow_log_var.get():
            self.log_text.see(tk.END)

    def trim_log_text(self) -> None:
        line_count = int(self.log_text.index("end-1c").split(".")[0])
        if line_count > MAX_LOG_LINES:
            delete_to = line_count - MAX_LOG_LINES
            self.log_text.delete("1.0", f"{delete_to}.0")

    def clear_log_text(self) -> None:
        self.log_text.configure(state=tk.NORMAL)
        self.log_text.delete("1.0", tk.END)
        self.log_text.configure(state=tk.DISABLED)

    @ui_guard("重新加载日志")
    def on_reload_log(self) -> None:
        self.read_log_incremental(force_reset=True)
        self.set_notice("日志已重新加载")

    @ui_guard("切换日志筛选")
    def on_log_filter_changed(self) -> None:
        self.read_log_incremental(force_reset=True)

    @ui_guard("清空日志显示")
    def on_clear_log_view(self) -> None:
        # 仅清空界面，不删除磁盘日志；随后从当前文件末尾继续。
        self.clear_log_text()

        if self.current_log_path and self.current_log_path.exists():
            self.current_log_offset = self.current_log_path.stat().st_size
        else:
            self.current_log_offset = 0

        self.set_notice("已清空界面显示，磁盘日志未删除")

    @ui_guard("打开日志目录")
    def on_open_log_dir(self) -> None:
        LOG_DIR.mkdir(parents=True, exist_ok=True)

        if shutil.which("xdg-open"):
            subprocess.Popen(
                ["xdg-open", str(LOG_DIR)],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                start_new_session=True,
            )
            return

        if shutil.which("explorer.exe") and shutil.which("wslpath"):
            result = run_command(
                ["wslpath", "-w", str(LOG_DIR)],
                timeout=3.0,
            )
            if result.returncode == 0:
                subprocess.Popen(
                    ["explorer.exe", result.stdout.strip()],
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
                return

        raise OperationError(
            f"无法自动打开目录，请手动访问：{LOG_DIR}"
        )

    # 配置编辑

    def refresh_config_file_list(self) -> None:
        if not CONFIG_DIR.is_dir():
            LOGGER.error("配置目录不存在：%s", CONFIG_DIR)
            self.config_box.configure(values=())
            return

        try:
            files = sorted(
                path.relative_to(CONFIG_DIR).as_posix()
                for path in CONFIG_DIR.rglob("*.toml")
                if path.is_file()
            )
        except OSError:
            LOGGER.exception("扫描配置文件失败")
            files = []

        self.config_box.configure(values=files)

    def load_initial_config(self) -> None:
        values = list(self.config_box.cget("values"))
        if not values:
            self.mode_hint_var.set("当前配置模式：配置目录中没有 TOML 文件")
            return

        initial = "main.toml" if "main.toml" in values else values[0]
        self.config_file_var.set(initial)
        self.load_config_file(initial)

    @ui_guard("选择配置文件")
    def on_config_selected(self, _event=None) -> None:
        selected = self.config_file_var.get()
        if not selected or selected == self.current_config_rel:
            return

        if self.config_dirty:
            discard = messagebox.askyesno(
                "未保存修改",
                "当前配置有未保存修改，是否放弃修改并切换文件？",
                parent=self.root,
            )
            if not discard:
                self.config_file_var.set(self.current_config_rel or "")
                return

        self.load_config_file(selected)

    def load_config_file(self, relative_path: str) -> None:
        path = self.safe_config_path(relative_path)

        try:
            text = path.read_text(encoding="utf-8")
        except OSError as exc:
            LOGGER.exception("读取配置失败：%s", path)
            raise OperationError(f"读取配置失败：{path}\n{exc}") from exc

        self.loading_config = True
        try:
            self.config_text.delete("1.0", tk.END)
            self.config_text.insert("1.0", text)
            self.config_text.edit_reset()
            self.config_text.edit_modified(False)
            self.current_config_rel = relative_path
            self.config_file_var.set(relative_path)
            self.config_dirty = False
        finally:
            self.loading_config = False

        self.update_mode_hint()
        self.set_notice(f"已加载配置：{relative_path}")

    def safe_config_path(self, relative_path: str) -> Path:
        candidate = (CONFIG_DIR / relative_path).resolve()
        config_root = CONFIG_DIR.resolve()

        try:
            candidate.relative_to(config_root)
        except ValueError as exc:
            LOGGER.error("拒绝访问配置目录外的路径：%s", candidate)
            raise OperationError("配置路径越界") from exc

        return candidate

    def on_config_modified(self, _event=None) -> None:
        if self.loading_config:
            self.config_text.edit_modified(False)
            return

        if self.config_text.edit_modified():
            self.config_dirty = True
            self.config_text.edit_modified(False)
            name = self.current_config_rel or "当前文件"
            self.set_notice(f"{name} 有未保存修改")

    @ui_guard("重新加载配置")
    def on_reload_config(self) -> None:
        if not self.current_config_rel:
            raise OperationError("尚未选择配置文件")

        if self.config_dirty:
            discard = messagebox.askyesno(
                "放弃修改",
                "确定放弃当前未保存的修改吗？",
                parent=self.root,
            )
            if not discard:
                return

        self.load_config_file(self.current_config_rel)

    def parse_toml_text(self, text: str) -> dict:
        if tomllib is None:
            raise OperationError(
                "当前 Python 没有 TOML 解析器。\n"
                "请使用 Python 3.11+，或安装兼容包："
                "python3 -m pip install tomli"
            )

        try:
            return tomllib.loads(text)
        except Exception as exc:
            LOGGER.error("TOML 校验失败：%s", exc)
            raise OperationError(f"TOML 语法错误：\n{exc}") from exc

    @ui_guard("校验配置")
    def on_validate_config(self) -> None:
        text = self.config_text.get("1.0", "end-1c")
        self.parse_toml_text(text)
        self.set_notice("TOML 语法校验通过")
        messagebox.showinfo(
            "校验通过",
            "当前文件的 TOML 语法正确。\n"
            "注意：这不等于所有参数值都符合 C++ 程序要求。",
            parent=self.root,
        )

    def save_current_config(self) -> Path:
        if not self.current_config_rel:
            raise OperationError("尚未选择配置文件")

        path = self.safe_config_path(self.current_config_rel)
        text = self.config_text.get("1.0", "end-1c")

        # 先做语法校验，再触碰原文件。
        self.parse_toml_text(text)

        try:
            atomic_write_text(path, text + ("" if text.endswith("\n") else "\n"))
        except OSError as exc:
            LOGGER.exception("原子保存配置失败：%s", path)
            raise OperationError(f"保存配置失败：{path}\n{exc}") from exc

        self.config_dirty = False
        self.config_text.edit_modified(False)
        self.update_mode_hint()

        LOGGER.info("配置已保存：%s", path)
        return path

    @ui_guard("保存配置")
    def on_save_config(self) -> None:
        path = self.save_current_config()
        self.set_notice(f"已保存：{path.relative_to(PROJECT_ROOT)}")
        messagebox.showinfo(
            "保存成功",
            f"配置已保存：\n{path}\n\n"
            f"备份文件：{path.with_suffix(path.suffix + '.bak')}\n"
            "主程序需要重启后才能重新读取配置。",
            parent=self.root,
        )

    @ui_guard("保存并重启")
    def on_save_and_restart(self) -> None:
        path = self.save_current_config()
        mode = self.control_mode_var.get()

        self.run_background(
            "保存并重启",
            lambda: (
                f"已保存 {path.relative_to(PROJECT_ROOT)}；"
                f"{self.restart_backend(mode)}"
            ),
        )

    def update_mode_hint(self) -> None:
        main_path = CONFIG_DIR / "main.toml"

        try:
            text = main_path.read_text(encoding="utf-8")
            data = self.parse_toml_text(text)
            mode_data = data.get("mode", {})
            enabled = bool(mode_data.get("enabled", False))
            name = str(mode_data.get("name", "")).strip() or "未设置"

            if enabled:
                mode_path = CONFIG_DIR / "modes" / f"{name}.toml"
                suffix = "存在" if mode_path.is_file() else "不存在"
                self.mode_hint_var.set(
                    f"当前配置模式：{name}（已启用，"
                    f"最高优先级文件 modes/{name}.toml {suffix}）"
                )
            else:
                self.mode_hint_var.set("当前配置模式：未启用")
        except Exception as exc:
            LOGGER.warning("读取当前模式失败：%s", exc, exc_info=True)
            self.mode_hint_var.set(f"当前配置模式：读取失败（{exc}）")

    # GUI 偏好存取

    def _load_gui_prefs(self) -> None:
        try:
            if GUI_PREFS_FILE.is_file():
                data = json.loads(GUI_PREFS_FILE.read_text(encoding="utf-8"))
                size = data.get("font_size", 10)
                if 8 <= size <= 24:
                    self.font_size_var.set(size)
        except Exception:
            LOGGER.warning("读取 GUI 偏好失败，使用默认值", exc_info=True)

    def _save_gui_prefs(self) -> None:
        try:
            data = {"font_size": self.font_size_var.get()}
            GUI_PREFS_FILE.parent.mkdir(parents=True, exist_ok=True)
            atomic_write_text(GUI_PREFS_FILE, json.dumps(data, indent=2) + "\n", make_backup=False)
        except Exception:
            LOGGER.warning("保存 GUI 偏好失败", exc_info=True)

    def _on_font_size_changed(self, *_args) -> None:
        size = self.font_size_var.get()
        self.log_text.configure(font=("TkFixedFont", size))
        self.config_text.configure(font=("TkFixedFont", size + 1))
        self._save_gui_prefs()

    # UI 状态与关闭

    def set_control_buttons_enabled(self, enabled: bool) -> None:
        state = tk.NORMAL if enabled else tk.DISABLED
        self.start_button.configure(state=state)
        self.stop_button.configure(state=state)
        self.restart_button.configure(state=state)
        self.refresh_button.configure(state=state)

    # ── 标定 Tab ──

    def build_calibration_tab(self) -> None:
        """标定页：ROI 配置子页面 + 位置标定子页面（第二阶段）。"""
        self.calib_notebook = ttk.Notebook(self.calib_tab)
        self.calib_notebook.pack(fill=tk.BOTH, expand=True)

        self.calib_roi_tab = ttk.Frame(self.calib_notebook, padding=8)
        self.calib_axis_tab = ttk.Frame(self.calib_notebook, padding=8)
        self.calib_notebook.add(self.calib_roi_tab, text="ROI 配置")
        self.calib_notebook.add(self.calib_axis_tab, text="位置标定")

        self._build_roi_tab()
        self._build_axis_tab()

    # ── ROI 配置子页面 ──

    def _build_roi_tab(self) -> None:
        toolbar = ttk.Frame(self.calib_roi_tab)
        toolbar.pack(fill=tk.X)

        ttk.Label(toolbar, text="编辑检测区域：").pack(side=tk.LEFT)
        self.calib_model_var = tk.StringVar(value="full")
        calib_model_box = ttk.Combobox(
            toolbar,
            textvariable=self.calib_model_var,
            values=("full", "center"),
            state="readonly",
            width=12,
        )
        calib_model_box.pack(side=tk.LEFT, padx=(4, 12))
        calib_model_box.bind("<<ComboboxSelected>>", self._calib_on_model_changed)

        ttk.Label(toolbar, text="工具：").pack(side=tk.LEFT)
        self.calib_tool_var = tk.StringVar(value="select")
        calib_tool_box = ttk.Combobox(
            toolbar,
            textvariable=self.calib_tool_var,
            values=("select", "draw"),
            state="readonly",
            width=10,
        )
        calib_tool_box.pack(side=tk.LEFT, padx=(4, 12))
        calib_tool_box.bind("<<ComboboxSelected>>", self._calib_on_tool_changed)

        self.calib_draw_btn = ttk.Button(
            toolbar, text="重新框选", command=self._calib_start_draw
        )
        self.calib_draw_btn.pack(side=tk.LEFT, padx=3)

        ttk.Separator(toolbar, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=12, pady=2)

        self.calib_lock_aspect_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(
            toolbar,
            text="锁定模型宽高比",
            variable=self.calib_lock_aspect_var,
        ).pack(side=tk.LEFT, padx=(0, 12))

        ttk.Separator(toolbar, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=12, pady=2)

        ttk.Button(
            toolbar, text="铺满画面", command=self._calib_fill_frame
        ).pack(side=tk.LEFT, padx=3)

        ttk.Button(
            toolbar, text="恢复配置", command=self._calib_reload
        ).pack(side=tk.LEFT, padx=3)

        ttk.Button(
            toolbar, text="保存 ROI", command=self._calib_save
        ).pack(side=tk.LEFT, padx=3)

        ttk.Button(
            toolbar, text="打开摄像头", command=self._calib_start_camera
        ).pack(side=tk.LEFT, padx=3)

        ttk.Button(
            toolbar, text="停止摄像头", command=self._calib_stop_camera
        ).pack(side=tk.LEFT, padx=3)

        self.calib_info_var = tk.StringVar(value="未启动摄像头")
        ttk.Label(
            toolbar,
            textvariable=self.calib_info_var,
            anchor=tk.W,
        ).pack(side=tk.LEFT, padx=(12, 0))

        # 主布局：Canvas 左 + 输入面板右
        main_frame = ttk.Frame(self.calib_roi_tab)
        main_frame.pack(fill=tk.BOTH, expand=True, pady=(8, 0))

        canvas_frame = ttk.Frame(main_frame)
        canvas_frame.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

        self.calib_canvas = tk.Canvas(
            canvas_frame,
            bg="black",
            highlightthickness=0,
        )
        self.calib_canvas.pack(fill=tk.BOTH, expand=True)

        # 右侧输入面板
        input_frame = ttk.LabelFrame(main_frame, text="当前模型参数", padding=8)
        input_frame.pack(side=tk.RIGHT, fill=tk.Y, padx=(8, 0))

        ttk.Label(input_frame, text="检测区域：").grid(
            row=0, column=0, sticky=tk.E, padx=(0, 4)
        )
        self.calib_model_label_var = tk.StringVar(value="Full 模型")
        ttk.Label(input_frame, textvariable=self.calib_model_label_var, font=("", 10, "bold")).grid(
            row=0, column=1, sticky=tk.W, pady=(0, 4)
        )

        ttk.Label(input_frame, text="ROI 原图坐标", font=("", 10, "bold")).grid(
            row=1, column=0, columnspan=2, sticky=tk.W, pady=(8, 4)
        )

        ttk.Label(input_frame, text="X:").grid(row=2, column=0, sticky=tk.E, padx=(0, 4))
        self.roi_x_var = tk.IntVar()
        self.roi_x_entry = ttk.Entry(input_frame, textvariable=self.roi_x_var, width=7)
        self.roi_x_entry.grid(row=2, column=1, sticky=tk.W)
        self.roi_x_entry.bind("<Return>", lambda e: self._calib_apply_entry_values())
        self.roi_x_entry.bind("<FocusOut>", lambda e: self._calib_apply_entry_values())

        ttk.Label(input_frame, text="Y:").grid(row=3, column=0, sticky=tk.E, padx=(0, 4))
        self.roi_y_var = tk.IntVar()
        self.roi_y_entry = ttk.Entry(input_frame, textvariable=self.roi_y_var, width=7)
        self.roi_y_entry.grid(row=3, column=1, sticky=tk.W)
        self.roi_y_entry.bind("<Return>", lambda e: self._calib_apply_entry_values())
        self.roi_y_entry.bind("<FocusOut>", lambda e: self._calib_apply_entry_values())

        ttk.Label(input_frame, text="宽度:").grid(row=4, column=0, sticky=tk.E, padx=(0, 4))
        self.roi_width_var = tk.IntVar()
        self.roi_width_entry = ttk.Entry(input_frame, textvariable=self.roi_width_var, width=7)
        self.roi_width_entry.grid(row=4, column=1, sticky=tk.W)
        self.roi_width_entry.bind("<Return>", lambda e: self._calib_apply_entry_values())
        self.roi_width_entry.bind("<FocusOut>", lambda e: self._calib_apply_entry_values())

        ttk.Label(input_frame, text="高度:").grid(row=5, column=0, sticky=tk.E, padx=(0, 4))
        self.roi_height_var = tk.IntVar()
        self.roi_height_entry = ttk.Entry(input_frame, textvariable=self.roi_height_var, width=7)
        self.roi_height_entry.grid(row=5, column=1, sticky=tk.W)
        self.roi_height_entry.bind("<Return>", lambda e: self._calib_apply_entry_values())
        self.roi_height_entry.bind("<FocusOut>", lambda e: self._calib_apply_entry_values())

        ttk.Button(
            input_frame, text="应用参数", command=self._calib_apply_entry_values
        ).grid(row=6, column=0, columnspan=2, pady=(8, 12))

        ttk.Separator(input_frame, orient=tk.HORIZONTAL).grid(
            row=7, column=0, columnspan=2, sticky=tk.EW, pady=(0, 8)
        )

        ttk.Label(input_frame, text="模型输入尺寸", font=("", 10, "bold")).grid(
            row=8, column=0, columnspan=2, sticky=tk.W, pady=(0, 4)
        )
        self.calib_model_input_var = tk.StringVar(value="640 × 160")
        ttk.Label(input_frame, textvariable=self.calib_model_input_var).grid(
            row=9, column=0, columnspan=2, sticky=tk.W, pady=(0, 8)
        )

        ttk.Label(input_frame, text="ROI 比例", font=("", 10, "bold")).grid(
            row=10, column=0, columnspan=2, sticky=tk.W, pady=(0, 4)
        )
        self.calib_ratio_var = tk.StringVar(value="4.000")
        ttk.Label(input_frame, textvariable=self.calib_ratio_var).grid(
            row=11, column=0, columnspan=2, sticky=tk.W
        )

        # ── Q3～Q5 任务启动标定线（竖直线，只比较球心全局 X）──
        ttk.Separator(input_frame, orient=tk.HORIZONTAL).grid(
            row=12, column=0, columnspan=2, sticky=tk.EW, pady=(12, 8)
        )
        ttk.Label(input_frame, text="任务启动标定线", font=("", 10, "bold")).grid(
            row=13, column=0, columnspan=2, sticky=tk.W, pady=(0, 4)
        )

        ttk.Label(input_frame, text="标定线 X:").grid(
            row=14, column=0, sticky=tk.E, padx=(0, 4)
        )
        self.calibration_line_x_var = tk.IntVar()
        self.calibration_line_x_entry = ttk.Entry(
            input_frame,
            textvariable=self.calibration_line_x_var,
            width=7,
        )
        self.calibration_line_x_entry.grid(row=14, column=1, sticky=tk.W)
        self.calibration_line_x_entry.bind(
            "<Return>", lambda e: self._calib_apply_calibration_line()
        )
        self.calibration_line_x_entry.bind(
            "<FocusOut>", lambda e: self._calib_apply_calibration_line()
        )

        ttk.Label(input_frame, text="允许偏差 ±px:").grid(
            row=15, column=0, sticky=tk.E, padx=(0, 4)
        )
        self.calibration_line_tolerance_var = tk.IntVar()
        self.calibration_line_tolerance_entry = ttk.Entry(
            input_frame,
            textvariable=self.calibration_line_tolerance_var,
            width=7,
        )
        self.calibration_line_tolerance_entry.grid(
            row=15, column=1, sticky=tk.W
        )
        self.calibration_line_tolerance_entry.bind(
            "<Return>", lambda e: self._calib_apply_calibration_line()
        )
        self.calibration_line_tolerance_entry.bind(
            "<FocusOut>", lambda e: self._calib_apply_calibration_line()
        )

        ttk.Label(
            input_frame,
            text=(
                "用于 Q3～Q5 启动标定\n"
                "只比较球心 X，Y 不受限制"
            ),
            foreground="#666666",
            font=("", 8),
        ).grid(row=16, column=0, columnspan=2, sticky=tk.W, pady=(2, 0))

        # ── 摄像头（必须在数据模型同步之前初始化，防止 _calib_redraw 访问未定义属性）──
        self.calib_camera: Optional[CalibrationCamera] = None
        self.calib_running = False
        self.calib_current_frame = None
        self.calib_after_id: Optional[str] = None

        # ── 数据模型 ──
        self._roi_calibration = RoiCalibration()
        self._calib_load_from_config()

        # ── 同步保护标志 ──
        self._calib_syncing = False

        # 同步到输入框
        self._calib_sync_vars_from_model()

        # ── 拖动/交互状态 ──
        self.calib_drag_handle: RoiHandle = RoiHandle.NONE
        self.calib_drag_roi_key: Optional[str] = None
        self.calib_drag_original: Optional[RoiRect] = None
        self.calib_drag_start_x = 0.0
        self.calib_drag_start_y = 0.0
        self.roi_create_start: Optional[tuple[float, float]] = None
        self.calib_transform: Optional[CanvasTransform] = None

        # ── 位置标定状态 ──
        self.axis_calibration = AxisCalibration()
        self.axis_worker: Optional[CalibrationWorkerClient] = None
        self.axis_sampling = False
        self.axis_cancel_event = threading.Event()
        self.axis_dirty = False
        self.roi_dirty = False
        self._axis_control_widgets: list = []

        # ── Canvas 绑定 ──
        self.calib_canvas.bind("<ButtonPress-1>", self._calib_on_press)
        self.calib_canvas.bind("<B1-Motion>", self._calib_on_drag)
        self.calib_canvas.bind("<ButtonRelease-1>", self._calib_on_release)
        self.calib_canvas.bind("<Configure>", self._calib_on_resize)

        # 方向键微调（按住 Shift=10px，否则 1px）
        self.calib_canvas.bind("<Left>", lambda e: self._calib_nudge(-1, 0, e))
        self.calib_canvas.bind("<Right>", lambda e: self._calib_nudge(1, 0, e))
        self.calib_canvas.bind("<Up>", lambda e: self._calib_nudge(0, -1, e))
        self.calib_canvas.bind("<Down>", lambda e: self._calib_nudge(0, 1, e))
        self.calib_canvas.focus_set()

    # ── ROI 辅助 ──

    def _active_region(self) -> DetectorRegion:
        model_id = self.calib_model_var.get()
        return self._roi_calibration.active_region(model_id)

    def _other_region(self) -> DetectorRegion:
        model_id = self.calib_model_var.get()
        other = "center" if model_id == "full" else "full"
        return self._roi_calibration.active_region(other)

    # ── 数据模型 ⇄ 输入框 ──

    def _calib_sync_vars_from_model(self) -> None:
        """把当前模型的 ROI 参数同步到输入框。"""
        if self._calib_syncing:
            return

        self._calib_syncing = True
        try:
            region = self._active_region()
            roi = region.roi

            self.roi_x_var.set(roi.x)
            self.roi_y_var.set(roi.y)
            self.roi_width_var.set(roi.width)
            self.roi_height_var.set(roi.height)

            self.calibration_line_x_var.set(
                self._roi_calibration.calibration_line_x
            )
            self.calibration_line_tolerance_var.set(
                self._roi_calibration.calibration_line_tolerance_px
            )

            self.calib_model_label_var.set(region.display_name)
            self.calib_model_input_var.set(
                f"{region.input_width} × {region.input_height}"
            )
            self.calib_ratio_var.set(f"{region.aspect_ratio:.3f}")
        finally:
            self._calib_syncing = False

    def _calib_apply_entry_values(self, *_args) -> None:
        """把输入框值应用到当前模型 ROI，并进行边界钳制。"""
        if self._calib_syncing:
            return

        rc = self._roi_calibration
        model_id = self.calib_model_var.get()
        region = rc.active_region(model_id)
        roi = region.roi

        try:
            x = self.roi_x_var.get()
            y = self.roi_y_var.get()
            width = self.roi_width_var.get()
            height = self.roi_height_var.get()
        except tk.TclError:
            return

        if self.calib_lock_aspect_var.get():
            # 以宽度为主，自动计算高度
            height = round(width / region.aspect_ratio)
            if height < RoiRect.MIN_HEIGHT:
                height = RoiRect.MIN_HEIGHT
                width = round(height * region.aspect_ratio)

        if width < RoiRect.MIN_WIDTH:
            width = RoiRect.MIN_WIDTH
        if height < RoiRect.MIN_HEIGHT:
            height = RoiRect.MIN_HEIGHT

        roi.x = x
        roi.y = y
        roi.width = width
        roi.height = height

        rc.clamp_all()
        self._calib_sync_vars_from_model()
        self._calib_redraw()

    def _calib_apply_calibration_line(self, *_args) -> None:
        """把标定线输入框值应用到数据模型。"""
        if self._calib_syncing:
            return

        rc = self._roi_calibration
        try:
            line_x = self.calibration_line_x_var.get()
            tolerance = self.calibration_line_tolerance_var.get()
        except tk.TclError:
            return

        rc.calibration_line_x = line_x
        rc.calibration_line_tolerance_px = tolerance
        rc.clamp_all()
        self._calib_sync_vars_from_model()
        self._calib_redraw()

    def _calib_nudge(self, dx: int, dy: int, event: tk.Event) -> None:
        """方向键微调当前模型的 ROI 位置。"""
        step = 10 if (event.state & 0x0001) else 1  # Shift 键
        roi = self._active_region().roi
        roi.x += dx * step
        roi.y += dy * step

        self._roi_calibration.clamp_all()
        self._calib_sync_vars_from_model()
        self._calib_redraw()

    # ── 工具栏操作 ──

    @ui_guard("切换模型")
    def _calib_on_model_changed(self, _event=None) -> None:
        self._calib_sync_vars_from_model()
        self._calib_redraw()
        self._calib_update_info()

    @ui_guard("切换工具")
    def _calib_on_tool_changed(self, _event=None) -> None:
        tool = self.calib_tool_var.get()
        if tool == "select":
            self._calib_info_var_set("选择/移动模式 — 拖动矩形内部移动，拖动四角缩放")
        elif tool == "draw":
            self._calib_info_var_set("框选模式 — 在画面上拖出新的 ROI")

    def _calib_start_draw(self) -> None:
        """进入重新框选模式。"""
        self.calib_tool_var.set("draw")
        self._calib_info_var_set("框选模式 — 在画面上按住并拖出新的 ROI（松开后自动回到选择模式）")

    def _calib_fill_frame(self) -> None:
        """将当前 ROI 铺满整个画面（保持宽高比，纵向居中）。"""
        rc = self._roi_calibration
        region = self._active_region()
        roi = region.roi

        frame_w = rc.frame_width
        frame_h = rc.frame_height

        width = frame_w
        height = round(width / region.aspect_ratio)

        if height > frame_h:
            height = frame_h
            width = round(height * region.aspect_ratio)

        roi.x = (frame_w - width) // 2
        roi.y = (frame_h - height) // 2
        roi.width = width
        roi.height = height

        rc.clamp_all()
        self._calib_sync_vars_from_model()
        self._calib_redraw()
        self._calib_info_var_set(f"已将 {region.display_name} ROI 扩至最大可用范围")

    # ── 配置读写 ──

    def _load_camera_settings(self) -> tuple[int, int, int, str]:
        """从 config/camera.toml 读取摄像头分辨率、帧率、编码格式。"""
        camera_path = CONFIG_DIR / "camera.toml"

        width = 1280
        height = 720
        fps = 30
        fourcc = "MJPG"

        if not camera_path.is_file() or tomllib is None:
            return width, height, fps, fourcc

        with camera_path.open("rb") as file:
            document = tomllib.load(file)

        camera = document.get("camera", {})

        width = int(camera.get("width", width))
        height = int(camera.get("height", height))
        fps = int(camera.get("fps", fps))
        fourcc = str(camera.get("fourcc", fourcc))

        if width <= 0 or height <= 0:
            raise OperationError(
                f"camera.toml 中的画面尺寸非法：{width}×{height}"
            )

        return width, height, fps, fourcc

    def _calib_load_from_config(self) -> None:
        """从 camera.toml 和 vision.toml 读取当前的标定坐标系与 ROI 参数。"""
        rc = self._roi_calibration

        # ROI 的全局坐标系由摄像头配置决定，不是由模型输入尺寸决定。
        camera_width, camera_height, _, _ = self._load_camera_settings()
        rc.frame_width = camera_width
        rc.frame_height = camera_height

        vision_path = CONFIG_DIR / "vision.toml"
        try:
            if vision_path.is_file():
                with vision_path.open("rb") as f:
                    data = tomllib.load(f) if tomllib else {}
                bn = data.get("vision", {}).get("ball_ncnn", {})

                rc.full_roi.x = int(bn.get("full_roi_x", 0))
                rc.full_roi.y = int(bn.get("full_roi_y", 160))
                rc.full_roi.width = int(bn.get("full_src_width", 1280))
                rc.full_roi.height = int(bn.get("full_src_height", 320))

                rc.center_roi.x = int(bn.get("center_roi_x", 416))
                rc.center_roi.y = int(bn.get("center_roi_y", 160))
                rc.center_roi.width = int(bn.get("center_src_width", 448))
                rc.center_roi.height = int(bn.get("center_src_height", 320))

                rc.calibration_line_x = int(
                    bn.get(
                        "calibration_line_x",
                        bn.get("pipe_center_x", 640),
                    )
                )

                rc.calibration_line_tolerance_px = int(
                    bn.get(
                        "calibration_line_tolerance_px",
                        20,
                    )
                )

                rc.full_input_size = (
                    int(bn.get("full_input_width", 640)),
                    int(bn.get("full_input_height", 160)),
                )
                rc.center_input_size = (
                    int(bn.get("center_input_width", 224)),
                    int(bn.get("center_input_height", 160)),
                )

                rc.clamp_all()

                LOGGER.info(
                    "标定配置加载完成：frame=%dx%d, "
                    "full=(%d,%d,%d,%d), "
                    "center=(%d,%d,%d,%d)",
                    rc.frame_width, rc.frame_height,
                    rc.full_roi.x, rc.full_roi.y,
                    rc.full_roi.width, rc.full_roi.height,
                    rc.center_roi.x, rc.center_roi.y,
                    rc.center_roi.width, rc.center_roi.height,
                )
        except Exception:
            LOGGER.warning("从配置加载标定参数失败，使用默认值", exc_info=True)
            raise

    @ui_guard("重新加载标定配置")
    def _calib_reload(self) -> None:
        self._calib_load_from_config()
        self._calib_sync_vars_from_model()
        self._calib_info_var_set("已从配置重新加载标定参数")
        self._calib_redraw()
        self.set_notice("标定参数已重新加载")

    @ui_guard("保存标定")
    def _calib_save(self) -> None:
        if tomlkit is None:
            raise OperationError(
                "保存标定需要 tomlkit。\n"
                "运行：python3 -m pip install tomlkit"
            )

        vision_path = CONFIG_DIR / "vision.toml"

        try:
            text = vision_path.read_text(encoding="utf-8")
        except OSError as exc:
            raise OperationError(f"读取 {vision_path} 失败：{exc}") from exc

        document = tomlkit.parse(text)
        rc = self._roi_calibration
        rc.validate()

        ball_ncnn = document.get("vision", {}).get("ball_ncnn")  # type: ignore[union-attr]
        if ball_ncnn is None:
            raise OperationError("vision.toml 中缺少 [vision.ball_ncnn] 配置节")

        # ── 保存前检查 Full/Center ROI 关系 ──
        warnings: list[str] = []
        full = rc.full_roi
        center = rc.center_roi

        if not (
            center.x >= full.x
            and center.y >= full.y
            and center.right <= full.right
            and center.bottom <= full.bottom
        ):
            warnings.append("Center ROI 不完全位于 Full ROI 内，可能影响重捕获。")

        if full.width < center.width:
            warnings.append("Full ROI 比 Center ROI 更窄，可能影响重捕获。")

        if warnings:
            warning_text = "\n".join(warnings)
            proceed = messagebox.askyesno(
                "ROI 配置警告",
                f"以下问题可能影响检测效果：\n\n{warning_text}\n\n"
                "是否仍然保存？",
                parent=self.root,
            )
            if not proceed:
                return

        ball_ncnn["roi_location_mode"] = "topleft"
        ball_ncnn["full_roi_x"] = rc.full_roi.x
        ball_ncnn["full_roi_y"] = rc.full_roi.y
        ball_ncnn["full_src_width"] = rc.full_roi.width
        ball_ncnn["full_src_height"] = rc.full_roi.height

        ball_ncnn["center_roi_x"] = rc.center_roi.x
        ball_ncnn["center_roi_y"] = rc.center_roi.y
        ball_ncnn["center_src_width"] = rc.center_roi.width
        ball_ncnn["center_src_height"] = rc.center_roi.height

        ball_ncnn["calibration_line_x"] = rc.calibration_line_x
        ball_ncnn["calibration_line_tolerance_px"] = (
            rc.calibration_line_tolerance_px
        )

        rendered = tomlkit.dumps(document)

        # 保存后再解析一次，防止写出非法 TOML
        if tomllib is not None:
            try:
                tomllib.loads(rendered)
            except Exception as exc:
                raise OperationError(f"保存后 TOML 解析失败：{exc}") from exc

        atomic_write_text(vision_path, rendered)

        self.set_notice(
            f"标定参数已保存到 {vision_path.relative_to(PROJECT_ROOT)}；"
            "主程序重启后生效。"
        )
        LOGGER.info(
            "ROI/标定线已保存: "
            "full=(%d,%d,%d,%d) "
            "center=(%d,%d,%d,%d) "
            "calibration_line_x=%d tolerance=%d",
            rc.full_roi.x, rc.full_roi.y, rc.full_roi.width, rc.full_roi.height,
            rc.center_roi.x, rc.center_roi.y, rc.center_roi.width, rc.center_roi.height,
            rc.calibration_line_x,
            rc.calibration_line_tolerance_px,
        )

        # 同步刷新"配置"页面
        if self.current_config_rel == "vision.toml":
            self.load_config_file("vision.toml")

    # ── 摄像头 ──

    @ui_guard("打开摄像头")
    def _calib_start_camera(self) -> None:
        if not HAS_CV2:
            raise OperationError(
                "标定功能需要 opencv-python。\n"
                "运行：pip3 install opencv-python"
            )

        if self.calib_running:
            self.set_notice("摄像头已在运行")
            return

        width, height, fps, fourcc = self._load_camera_settings()

        self.calib_camera = CalibrationCamera(
            0,
            width=width,
            height=height,
            fps=fps,
            fourcc=fourcc,
            strict_size=True,
        )

        self.calib_camera.start()

        self.calib_running = True

        actual_w, actual_h = self.calib_camera.actual_size

        self._calib_info_var_set(
            "摄像头已启动："
            f"{actual_w}×{actual_h}；"
            "可拖动、缩放或重新框选 ROI"
        )

        self._calib_update_preview()

    @ui_guard("停止摄像头")
    def _calib_stop_camera(self) -> None:
        self.calib_running = False

        if self.calib_after_id is not None:
            try:
                self.root.after_cancel(self.calib_after_id)
            except tk.TclError:
                pass
            self.calib_after_id = None

        if self.calib_camera is not None:
            self.calib_camera.stop()
            self.calib_camera = None

        self.calib_current_frame = None
        self.calib_transform = None
        self.calib_canvas.delete("all")
        self._calib_info_var_set("摄像头已停止")

    def _calib_update_preview(self) -> None:
        if not self.calib_running or self.calib_camera is None:
            return

        frame = self.calib_camera.latest_frame()
        if frame is not None:
            self.calib_current_frame = frame
            self._calib_render_frame(frame)

        self.calib_after_id = self.root.after(
            33,
            self._calib_update_preview,
        )

    # ── Canvas 渲染 ──

    def _calib_make_transform(self, frame_h: int, frame_w: int) -> CanvasTransform:
        canvas_w = self.calib_canvas.winfo_width()
        canvas_h = self.calib_canvas.winfo_height()
        if canvas_w < 10 or canvas_h < 10:
            canvas_w = 640
            canvas_h = 320

        return CanvasTransform(
            frame_width=frame_w,
            frame_height=frame_h,
            canvas_width=canvas_w,
            canvas_height=canvas_h,
        )

    def _calib_render_frame(self, frame) -> None:
        frame_h, frame_w = frame.shape[:2]

        rc = self._roi_calibration

        # 只检查，绝对不能修改配置坐标系。
        if (
            frame_w != rc.frame_width
            or frame_h != rc.frame_height
        ):
            self._calib_info_var_set(
                "预览帧尺寸不匹配："
                f"实际 {frame_w}×{frame_h}，"
                f"标定坐标系 {rc.frame_width}"
                f"×{rc.frame_height}"
            )
            return

        transform = self._calib_make_transform(frame_h, frame_w)
        self.calib_transform = transform

        small = cv2.resize(frame, (transform.draw_width, transform.draw_height))

        rgb = cv2.cvtColor(small, cv2.COLOR_BGR2RGB)
        ppm_header = f"P6\n{transform.draw_width} {transform.draw_height}\n255\n".encode("ascii")
        ppm_data = ppm_header + rgb.tobytes()
        self.calib_tk_img = tk.PhotoImage(data=ppm_data)

        self.calib_canvas.delete("all")
        self.calib_canvas.create_image(
            transform.offset_x,
            transform.offset_y,
            image=self.calib_tk_img,
            anchor=tk.NW,
            tags="camera_frame",
        )

        self._calib_draw_overlay()

    def _calib_draw_overlay(self) -> None:
        transform = self.calib_transform
        if transform is None:
            return

        GREEN = "#00ff00"
        GREEN_DIM = "#338833"
        OVERLAY_FILL = "#000000"
        HANDLE_SIZE = 6

        rc = self._roi_calibration
        active_region = self._active_region()
        other_region = self._other_region()
        active_roi = active_region.roi
        other_roi = other_region.roi

        # ── 非活跃 ROI 外区域压暗 ──
        # 上边
        self.calib_canvas.create_rectangle(
            0, 0,
            self.calib_canvas.winfo_width(),
            transform.image_to_canvas(0, active_roi.top)[1],
            fill=OVERLAY_FILL, stipple="gray50", outline="", tags="overlay",
        )
        # 下边
        self.calib_canvas.create_rectangle(
            0, transform.image_to_canvas(0, active_roi.bottom)[1],
            self.calib_canvas.winfo_width(),
            self.calib_canvas.winfo_height(),
            fill=OVERLAY_FILL, stipple="gray50", outline="", tags="overlay",
        )
        # 左边
        self.calib_canvas.create_rectangle(
            0, transform.image_to_canvas(0, active_roi.top)[1],
            transform.image_to_canvas(active_roi.left, 0)[0],
            transform.image_to_canvas(0, active_roi.bottom)[1],
            fill=OVERLAY_FILL, stipple="gray50", outline="", tags="overlay",
        )
        # 右边
        self.calib_canvas.create_rectangle(
            transform.image_to_canvas(active_roi.right, 0)[0],
            transform.image_to_canvas(0, active_roi.top)[1],
            self.calib_canvas.winfo_width(),
            transform.image_to_canvas(0, active_roi.bottom)[1],
            fill=OVERLAY_FILL, stipple="gray50", outline="", tags="overlay",
        )

        # ── 当前模型 ROI：粗实线 ──
        ax, ay = transform.image_to_canvas(active_roi.x, active_roi.y)
        ax2, ay2 = transform.image_to_canvas(active_roi.right, active_roi.bottom)
        self.calib_canvas.create_rectangle(
            ax, ay, ax2, ay2,
            outline=GREEN, width=3, tags="active_roi",
        )

        # ── 另一个模型 ROI：细虚线 ──
        ox, oy = transform.image_to_canvas(other_roi.x, other_roi.y)
        ox2, oy2 = transform.image_to_canvas(other_roi.right, other_roi.bottom)
        self.calib_canvas.create_rectangle(
            ox, oy, ox2, oy2,
            outline=GREEN_DIM, width=1, dash=(8, 4), tags="other_roi",
        )

        # ── ROI 左上角标签 ──
        self.calib_canvas.create_text(
            ax + 4, ay + 4,
            text=active_region.display_name,
            anchor=tk.NW,
            fill=GREEN,
            font=("", 9, "bold"),
            tags="roi_label",
        )

        # ── ROI 尺寸标注 ──
        info_text = f"{active_roi.x}, {active_roi.y}, {active_roi.width}×{active_roi.height}"
        self.calib_canvas.create_text(
            ax, ay - 8,
            text=info_text,
            anchor=tk.SW,
            fill=GREEN,
            font=("", 8),
            tags="roi_info",
        )

        # ── 竖直标定线与有效带 ──
        line_x, line_top = transform.image_to_canvas(
            rc.calibration_line_x,
            rc.full_roi.top,
        )
        _, line_bottom = transform.image_to_canvas(
            rc.calibration_line_x,
            rc.full_roi.bottom,
        )

        left_x, _ = transform.image_to_canvas(
            rc.calibration_line_x
            - rc.calibration_line_tolerance_px,
            0,
        )
        right_x, _ = transform.image_to_canvas(
            rc.calibration_line_x
            + rc.calibration_line_tolerance_px,
            0,
        )

        # 有效带（半透明）
        self.calib_canvas.create_rectangle(
            left_x,
            line_top,
            right_x,
            line_bottom,
            fill="#00aaff",
            stipple="gray50",
            outline="",
            tags="calibration_line_band",
        )

        # 容差边界（虚线）
        self.calib_canvas.create_line(
            left_x,
            line_top,
            left_x,
            line_bottom,
            fill="#0088cc",
            width=1,
            dash=(4, 4),
            tags="calibration_line_band",
        )
        self.calib_canvas.create_line(
            right_x,
            line_top,
            right_x,
            line_bottom,
            fill="#0088cc",
            width=1,
            dash=(4, 4),
            tags="calibration_line_band",
        )

        # 主线（实线）
        self.calib_canvas.create_line(
            line_x,
            line_top,
            line_x,
            line_bottom,
            fill="#00aaff",
            width=3,
            tags="calibration_line",
        )

        # 标签
        self.calib_canvas.create_text(
            line_x + 5,
            line_top + 5,
            text=(
                f"启动标定线 X={rc.calibration_line_x} "
                f"±{rc.calibration_line_tolerance_px}px"
            ),
            anchor=tk.NW,
            fill="#00aaff",
            font=("", 9, "bold"),
            tags="calibration_line_label",
        )

        # ── 八方向缩放句柄（锁定宽高比时只显示四个角）──
        lock = self.calib_lock_aspect_var.get()
        handles = [
            (RoiHandle.NORTH_WEST, ax, ay),
            (RoiHandle.NORTH_EAST, ax2, ay),
            (RoiHandle.SOUTH_WEST, ax, ay2),
            (RoiHandle.SOUTH_EAST, ax2, ay2),
        ]
        if not lock:
            handles += [
                (RoiHandle.NORTH, (ax + ax2) / 2, ay),
                (RoiHandle.SOUTH, (ax + ax2) / 2, ay2),
                (RoiHandle.WEST, ax, (ay + ay2) / 2),
                (RoiHandle.EAST, ax2, (ay + ay2) / 2),
            ]

        for handle, hx, hy in handles:
            self.calib_canvas.create_rectangle(
                hx - HANDLE_SIZE, hy - HANDLE_SIZE,
                hx + HANDLE_SIZE, hy + HANDLE_SIZE,
                fill=GREEN, outline="#004400", tags="roi_handle",
            )

        # ── 另一个模型的标签 ──
        self.calib_canvas.create_text(
            ox + 4, oy + 4,
            text=other_region.display_name,
            anchor=tk.NW,
            fill=GREEN_DIM,
            font=("", 8),
            tags="other_label",
        )

    def _calib_redraw(self) -> None:
        """仅重绘叠加层（不清空画面）。"""
        self.calib_canvas.delete(
            "overlay", "active_roi", "other_roi",
            "roi_label", "roi_info", "other_label",
            "center_line", "roi_handle",
            "calibration_line", "calibration_line_band",
            "calibration_line_label",
        )
        if self.calib_current_frame is not None:
            self._calib_draw_overlay()

    # ── 鼠标事件 ──

    def _calib_hit_test(self, canvas_x: float, canvas_y: float) -> tuple[RoiHandle, Optional[str]]:
        """检测鼠标命中的 ROI 操作句柄，返回 (句柄类型, roi_key)。"""
        transform = self.calib_transform
        if transform is None:
            return RoiHandle.NONE, None

        image_x, image_y = transform.canvas_to_image(canvas_x, canvas_y)

        # 检测命中区域：当前模型 ROI
        active_region = self._active_region()
        active_roi = active_region.roi

        # 检查在哪个 ROI 内部
        in_active = active_roi.contains(image_x, image_y)

        # 检查句柄命中（当前模型）
        handle = self._calib_hit_handle(canvas_x, canvas_y, active_roi, active_region.model_id)
        if handle != RoiHandle.NONE:
            return handle, active_region.model_id

        # 命中内部 → 移动
        if in_active:
            return RoiHandle.MOVE, active_region.model_id

        return RoiHandle.NONE, None

    def _calib_hit_handle(
        self,
        canvas_x: float,
        canvas_y: float,
        roi: RoiRect,
        roi_key: str,
    ) -> RoiHandle:
        """检测鼠标是否命中指定 ROI 的缩放句柄。"""
        transform = self.calib_transform
        if transform is None:
            return RoiHandle.NONE

        ax, ay = transform.image_to_canvas(roi.x, roi.y)
        ax2, ay2 = transform.image_to_canvas(roi.right, roi.bottom)

        HIT = 14  # Canvas 像素命中半径

        lock = self.calib_lock_aspect_var.get()
        corners = {
            RoiHandle.NORTH_WEST: (ax, ay),
            RoiHandle.NORTH_EAST: (ax2, ay),
            RoiHandle.SOUTH_WEST: (ax, ay2),
            RoiHandle.SOUTH_EAST: (ax2, ay2),
        }

        for handle, (hx, hy) in corners.items():
            if abs(canvas_x - hx) < HIT and abs(canvas_y - hy) < HIT:
                return handle

        if not lock:
            edges = {
                RoiHandle.NORTH: ((ax + ax2) / 2, ay),
                RoiHandle.SOUTH: ((ax + ax2) / 2, ay2),
                RoiHandle.WEST: (ax, (ay + ay2) / 2),
                RoiHandle.EAST: (ax2, (ay + ay2) / 2),
            }
            for handle, (hx, hy) in edges.items():
                if abs(canvas_x - hx) < HIT and abs(canvas_y - hy) < HIT:
                    return handle

        return RoiHandle.NONE

    def _calib_on_press(self, event: tk.Event) -> None:
        transform = self.calib_transform
        if transform is None:
            return

        if not transform.contains_canvas_point(event.x, event.y):
            self.set_notice("鼠标位置不在有效图像区域", error=True)
            return

        image_x, image_y = transform.canvas_to_image(event.x, event.y)
        tool = self.calib_tool_var.get()

        if tool == "draw":
            # 重新框选模式
            self.roi_create_start = (image_x, image_y)
            self.calib_drag_handle = RoiHandle.NONE
            return

        # 选择/移动模式：命中测试
        handle, roi_key = self._calib_hit_test(event.x, event.y)
        self.calib_drag_handle = handle
        self.calib_drag_roi_key = roi_key

        if handle == RoiHandle.NONE:
            return

        self.calib_drag_start_x = image_x
        self.calib_drag_start_y = image_y

        region = self._active_region()
        roi = region.roi
        self.calib_drag_original = roi.copy()

    def _calib_on_drag(self, event: tk.Event) -> None:
        transform = self.calib_transform
        if transform is None:
            return

        if not transform.contains_canvas_point(event.x, event.y):
            return

        image_x, image_y = transform.canvas_to_image(event.x, event.y)
        tool = self.calib_tool_var.get()

        if tool == "draw" and self.roi_create_start is not None:
            # 框选模式：用 from_drag 动态创建 ROI
            start_x, start_y = self.roi_create_start
            region = self._active_region()
            aspect = region.aspect_ratio if self.calib_lock_aspect_var.get() else 0.0

            if aspect > 0:
                region.roi = RoiRect.from_drag(
                    start_x, start_y, image_x, image_y, aspect,
                )
            else:
                # 自由比例：任意矩形框选
                region.roi = RoiRect.from_points(
                    start_x, start_y, image_x, image_y,
                )

            self._roi_calibration.clamp_all()
            self._calib_sync_vars_from_model()
            self._calib_redraw()
            return

        # 选择/移动模式
        handle = self.calib_drag_handle
        if handle == RoiHandle.NONE or self.calib_drag_original is None:
            return

        rc = self._roi_calibration
        region = self._active_region()
        roi = region.roi
        original = self.calib_drag_original
        dx = round(image_x - self.calib_drag_start_x)
        dy = round(image_y - self.calib_drag_start_y)
        lock = self.calib_lock_aspect_var.get()

        if handle == RoiHandle.MOVE:
            roi.move_to(
                original.x + dx,
                original.y + dy,
                rc.frame_width,
                rc.frame_height,
            )
        elif handle in (
            RoiHandle.NORTH_WEST, RoiHandle.NORTH_EAST,
            RoiHandle.SOUTH_WEST, RoiHandle.SOUTH_EAST,
        ):
            self._calib_resize_roi(handle, original, dx, dy, region.aspect_ratio, lock)
        elif not lock and handle in (
            RoiHandle.NORTH, RoiHandle.SOUTH,
            RoiHandle.WEST, RoiHandle.EAST,
        ):
            self._calib_resize_edge(handle, original, dx, dy)

        rc.clamp_all()
        self._calib_sync_vars_from_model()
        self._calib_redraw()

    def _calib_resize_roi(
        self,
        handle: RoiHandle,
        original: RoiRect,
        dx: int,
        dy: int,
        aspect_ratio: float,
        lock: bool,
    ) -> None:
        """从四角等比例缩放 ROI。"""
        region = self._active_region()
        roi = region.roi

        if handle == RoiHandle.NORTH_WEST:
            new_right = original.right
            new_bottom = original.bottom
            new_left = original.x + dx
            new_top = original.y + dy
        elif handle == RoiHandle.NORTH_EAST:
            new_left = original.x
            new_bottom = original.bottom
            new_right = original.right + dx
            new_top = original.y + dy
        elif handle == RoiHandle.SOUTH_WEST:
            new_right = original.right
            new_top = original.y
            new_left = original.x + dx
            new_bottom = original.bottom + dy
        elif handle == RoiHandle.SOUTH_EAST:
            new_left = original.x
            new_top = original.y
            new_right = original.right + dx
            new_bottom = original.bottom + dy
        else:
            return

        if lock:
            # 等比例缩放：取宽高中变化更大的维度
            raw_w = new_right - new_left
            raw_h = new_bottom - new_top

            if abs(dx) >= abs(dy) or raw_w / max(aspect_ratio, 0.01) >= raw_h:
                width = max(RoiRect.MIN_WIDTH, raw_w)
                height = round(width / aspect_ratio)
            else:
                height = max(RoiRect.MIN_HEIGHT, raw_h)
                width = round(height * aspect_ratio)

            # 根据拖动的角重新计算位置
            if "WEST" in handle.value:
                roi.x = new_right - width
            else:
                roi.x = new_left

            if "NORTH" in handle.value:
                roi.y = new_bottom - height
            else:
                roi.y = new_top

            roi.width = width
            roi.height = height
        else:
            roi.x = new_left
            roi.y = new_top
            roi.width = max(RoiRect.MIN_WIDTH, new_right - new_left)
            roi.height = max(RoiRect.MIN_HEIGHT, new_bottom - new_top)

    def _calib_resize_edge(
        self,
        handle: RoiHandle,
        original: RoiRect,
        dx: int,
        dy: int,
    ) -> None:
        """从边中间句柄缩放（仅自由比例模式使用）。"""
        region = self._active_region()
        roi = region.roi

        if handle == RoiHandle.NORTH:
            new_top = original.y + dy
            new_bottom = original.bottom
            roi.y = min(new_top, new_bottom - RoiRect.MIN_HEIGHT)
            roi.height = max(RoiRect.MIN_HEIGHT, new_bottom - roi.y)
        elif handle == RoiHandle.SOUTH:
            roi.height = max(RoiRect.MIN_HEIGHT, original.height + dy)
        elif handle == RoiHandle.WEST:
            new_left = original.x + dx
            new_right = original.right
            roi.x = min(new_left, new_right - RoiRect.MIN_WIDTH)
            roi.width = max(RoiRect.MIN_WIDTH, new_right - roi.x)
        elif handle == RoiHandle.EAST:
            roi.width = max(RoiRect.MIN_WIDTH, original.width + dx)

    def _calib_on_release(self, event: tk.Event) -> None:
        # 框选完成：自动切回选择模式
        if self.calib_tool_var.get() == "draw":
            self.calib_tool_var.set("select")
            self._calib_info_var_set("框选完成")

        self.calib_drag_handle = RoiHandle.NONE
        self.calib_drag_roi_key = None
        self.calib_drag_original = None
        self.roi_create_start = None

    def _calib_on_resize(self, _event: tk.Event) -> None:
        if self.calib_current_frame is not None:
            self._calib_render_frame(self.calib_current_frame)

    def _calib_update_info(self) -> None:
        region = self._active_region()
        roi = region.roi
        tool = self.calib_tool_var.get()
        tool_name = "框选" if tool == "draw" else "选择/移动"
        self._calib_info_var_set(
            f"[{tool_name}] {region.display_name}: "
            f"({roi.x}, {roi.y}) {roi.width}×{roi.height}"
        )

    def _calib_info_var_set(self, text: str) -> None:
        self.calib_info_var.set(text)

    # ── 位置标定子页面 ──

    def _build_axis_tab(self) -> None:
        """位置标定页：采样控制 + 标定点表格 + 保存/验证/同步。"""
        # ── 上半：提示 + 采样控制 ──
        upper_frame = ttk.Frame(self.calib_axis_tab)
        upper_frame.pack(fill=tk.X, pady=(0, 8))

        ttk.Label(
            upper_frame,
            text="摄像头预览请查看「ROI 配置」页签。",
            foreground="#666666",
            font=("", 9),
        ).pack(anchor=tk.W)

        self.axis_cam_status_var = tk.StringVar(value="摄像头：未启动")
        ttk.Label(
            upper_frame,
            textvariable=self.axis_cam_status_var,
            foreground="#666666",
            font=("", 8),
        ).pack(anchor=tk.W, pady=(2, 6))

        control_frame = ttk.LabelFrame(upper_frame, text="采样控制", padding=8)
        control_frame.pack(fill=tk.X)

        # 第一行：物理位置 + 采样帧数 + 最低置信度
        row1 = ttk.Frame(control_frame)
        row1.pack(fill=tk.X, pady=(0, 4))

        ttk.Label(row1, text="已知物理位置：").pack(side=tk.LEFT)
        self.axis_position_var = tk.StringVar(value="-50.0")
        self.axis_position_entry = ttk.Entry(
            row1, textvariable=self.axis_position_var, width=9,
        )
        self.axis_position_entry.pack(side=tk.LEFT, padx=(2, 16))
        self._axis_control_widgets.append(self.axis_position_entry)

        ttk.Label(row1, text="采样帧数：").pack(side=tk.LEFT)
        self.axis_total_frames_var = tk.IntVar(value=30)
        self.axis_total_frames_spin = ttk.Spinbox(
            row1, from_=10, to=200, textvariable=self.axis_total_frames_var, width=5,
        )
        self.axis_total_frames_spin.pack(side=tk.LEFT, padx=(2, 16))
        self._axis_control_widgets.append(self.axis_total_frames_spin)

        ttk.Label(row1, text="最低置信度：").pack(side=tk.LEFT)
        self.axis_min_confidence_var = tk.DoubleVar(value=0.55)
        self.axis_min_confidence_spin = ttk.Spinbox(
            row1, from_=0.01, to=1.0, increment=0.05,
            textvariable=self.axis_min_confidence_var, width=5,
        )
        self.axis_min_confidence_spin.pack(side=tk.LEFT, padx=(2, 16))
        self._axis_control_widgets.append(self.axis_min_confidence_spin)

        ttk.Label(row1, text="最大 MAD px：").pack(side=tk.LEFT)
        self.axis_max_mad_var = tk.DoubleVar(value=1.5)
        self.axis_max_mad_spin = ttk.Spinbox(
            row1, from_=0.1, to=10.0, increment=0.1,
            textvariable=self.axis_max_mad_var, width=5,
        )
        self.axis_max_mad_spin.pack(side=tk.LEFT, padx=(2, 0))
        self._axis_control_widgets.append(self.axis_max_mad_spin)

        # 第二行：模型信息 + 状态 + 按钮
        row2 = ttk.Frame(control_frame)
        row2.pack(fill=tk.X)

        self.axis_model_label_var = tk.StringVar(value="检测模型：Full 模型")
        ttk.Label(row2, textvariable=self.axis_model_label_var).pack(side=tk.LEFT)

        self.axis_status_var = tk.StringVar(value="采样状态：未开始")
        ttk.Label(
            row2, textvariable=self.axis_status_var,
            foreground="#555555",
        ).pack(side=tk.LEFT, padx=(16, 0))

        ttk.Button(
            row2, text="开始采样", command=self._axis_start_sampling,
        ).pack(side=tk.RIGHT, padx=(4, 0))
        self._axis_control_widgets.append(
            row2.winfo_children()[-1]
        )

        ttk.Button(
            row2, text="取消采样", command=self._axis_cancel_sampling,
        ).pack(side=tk.RIGHT, padx=(4, 0))
        self._axis_control_widgets.append(
            row2.winfo_children()[-1]
        )

        # ── 下半：标定点表格 ──
        lower_frame = ttk.Frame(self.calib_axis_tab)
        lower_frame.pack(fill=tk.BOTH, expand=True)

        columns = ("序号", "位置 mm", "像素 X", "MAD px",
                   "峰峰值 px", "有效帧", "状态")
        self.axis_tree = ttk.Treeview(
            lower_frame, columns=columns, show="headings", height=8,
        )
        col_widths = [45, 80, 80, 70, 80, 70, 80]
        for col, width in zip(columns, col_widths):
            self.axis_tree.heading(col, text=col)
            self.axis_tree.column(col, width=width, anchor=tk.CENTER)

        tree_scroll = ttk.Scrollbar(
            lower_frame, orient=tk.VERTICAL, command=self.axis_tree.yview,
        )
        self.axis_tree.configure(yscrollcommand=tree_scroll.set)
        self.axis_tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        tree_scroll.pack(side=tk.RIGHT, fill=tk.Y)

        # ── 按钮行 ──
        btn_frame = ttk.Frame(lower_frame)
        btn_frame.pack(fill=tk.X, pady=(6, 2))

        btns_left = ttk.Frame(btn_frame)
        btns_left.pack(side=tk.LEFT)
        for text, cmd in [
            ("删除点", self._axis_delete_point),
            ("重新采样", self._axis_resample_point),
            ("清空", self._axis_clear_all),
        ]:
            btn = ttk.Button(btns_left, text=text, command=cmd)
            btn.pack(side=tk.LEFT, padx=2)
            self._axis_control_widgets.append(btn)

        btns_mid = ttk.Frame(btn_frame)
        btns_mid.pack(side=tk.LEFT, padx=(20, 0))
        for text, cmd in [
            ("用 0 mm 点更新启动线", self._axis_sync_line_to_zero),
            ("验证当前位置", self._axis_verify_position),
        ]:
            btn = ttk.Button(btns_mid, text=text, command=cmd)
            btn.pack(side=tk.LEFT, padx=2)
            self._axis_control_widgets.append(btn)

        btn = ttk.Button(
            btns_mid, text="保存位置标定",
            command=self._axis_save,
        )
        btn.pack(side=tk.LEFT, padx=2)
        self._axis_control_widgets.append(btn)

        # ── 0 mm 点与启动线关系 ──
        relation_frame = ttk.Frame(lower_frame)
        relation_frame.pack(fill=tk.X, pady=(6, 0))
        self.axis_line_info_var = tk.StringVar(
            value="0 mm 标定点像素：-  任务启动线 X：-  偏差：-",
        )
        ttk.Label(
            relation_frame,
            textvariable=self.axis_line_info_var,
            foreground="#444444",
            font=("", 9),
        ).pack(side=tk.LEFT)

        # ── 从配置加载已有标定点 ──
        self._axis_load_from_config()
        self._axis_update_cam_status()

    # ── 位置标定：Worker 管理 ──

    def _axis_start_worker(self) -> None:
        """启动标定 worker 子进程。"""
        if self.axis_worker is not None:
            return

        self.axis_worker = CalibrationWorkerClient(
            CONFIG_DIR,
            PROJECT_ROOT,
        )
        self.axis_worker.start()
        self.set_notice("标定 worker 已启动")

    def _axis_stop_worker(self) -> None:
        """停止标定 worker。"""
        if self.axis_worker is not None:
            self.axis_worker.stop()
            self.axis_worker = None

    # ── 位置标定：配置加载 ──

    @staticmethod
    def _parse_float_list(text: str) -> list[float]:
        """解析逗号分隔的浮点数列表。"""
        if not text or not text.strip():
            return []
        result = []
        for part in text.split(","):
            part = part.strip()
            if part:
                result.append(float(part))
        return result

    def _axis_load_from_config(self) -> None:
        """从 vision.toml 加载已有的轴标定点。"""
        vision_path = CONFIG_DIR / "vision.toml"
        if not vision_path.is_file() or tomllib is None:
            return

        try:
            with vision_path.open("rb") as f:
                data = tomllib.load(f)

            table = (
                data.get("vision", {})
                .get("ball_ncnn", {})
                .get("axis_calibration", {})
            )
            pixels = self._parse_float_list(table.get("pixels", ""))
            positions = self._parse_float_list(table.get("positions_mm", ""))

            if len(pixels) != len(positions):
                raise OperationError(
                    "pixels 和 positions_mm 数量不一致"
                )

            self.axis_calibration = AxisCalibration(
                image_right_sign=int(
                    table.get("image_right_sign", 1)
                ),
                points=[
                    AxisCalibrationPoint(
                        position_mm=pos,
                        pixel_x=pix,
                    )
                    for pix, pos in zip(pixels, positions)
                ],
            )
            self._axis_refresh_table()
            self._axis_update_line_info()
            LOGGER.info(
                "加载了 %d 个历史标定点",
                len(self.axis_calibration.points),
            )
        except Exception:
            LOGGER.warning("加载轴标定点失败", exc_info=True)

    # ── 位置标定：采样 ──

    def _axis_check_prerequisites(self) -> None:
        """采样前五项检查。"""
        if not self.calib_running or self.calib_camera is None:
            raise OperationError(
                "请先在「ROI 配置」页打开摄像头"
            )
        if self.roi_dirty:
            raise OperationError(
                "请先保存 ROI。位置采样必须使用与正式程序相同的 ROI。"
            )
        self._roi_calibration.validate()
        try:
            float(self.axis_position_var.get())
        except (ValueError, tk.TclError):
            raise OperationError("请输入有效的已知物理位置（数字）")

    def _axis_start_sampling(self) -> None:
        """按钮回调：启动后台采样线程。"""
        if self.axis_sampling:
            self.set_notice("已在采样中")
            return

        try:
            self._axis_check_prerequisites()
        except OperationError as exc:
            self.set_notice(f"无法开始采样：{exc}", error=True)
            messagebox.showerror("无法开始采样", str(exc), parent=self.root)
            return

        # 确保 worker 已启动
        if self.axis_worker is None:
            try:
                self._axis_start_worker()
            except Exception as exc:
                self.set_notice(f"worker 启动失败：{exc}", error=True)
                messagebox.showerror("Worker 启动失败", str(exc), parent=self.root)
                return

        self.axis_sampling = True
        self.axis_cancel_event.clear()
        self._set_axis_controls_enabled(False)

        position_mm = float(self.axis_position_var.get())
        total_frames = self.axis_total_frames_var.get()
        min_confidence = self.axis_min_confidence_var.get()
        max_mad = self.axis_max_mad_var.get()

        self.axis_status_var.set("采样状态：采样中…")

        threading.Thread(
            target=self._axis_sample_worker,
            args=(position_mm, total_frames, min_confidence, max_mad),
            daemon=True,
            name="axis-calibration-sampling",
        ).start()

    def _axis_cancel_sampling(self) -> None:
        """取消当前采样。"""
        if not self.axis_sampling:
            return
        self.axis_cancel_event.set()
        self.axis_status_var.set("采样状态：已取消")

    def _axis_sample_worker(
        self,
        position_mm: float,
        total_frames: int,
        min_confidence: float,
        max_mad: float,
    ) -> None:
        """采样工作线程（不在主线程中运行）。"""
        try:
            # 1. 重置检测器
            self.axis_worker.reset_tracking()

            # 2. 丢弃前 3 帧
            for _ in range(3):
                if self.axis_cancel_event.is_set():
                    self.post_ui(self._axis_sampling_cleanup, "已取消")
                    return
                frame = self.calib_camera.latest_frame()
                if frame is not None:
                    self.axis_worker.infer(frame, mode="FULL", timeout_s=2.0)

            # 3. 正式采集
            x_samples: list[float] = []
            confidences: list[float] = []

            for i in range(total_frames):
                if self.axis_cancel_event.is_set():
                    self.post_ui(self._axis_sampling_cleanup, "已取消")
                    return

                frame = self.calib_camera.latest_frame()
                if frame is None:
                    continue

                measurement = self.axis_worker.infer(
                    frame, mode="FULL", timeout_s=2.0,
                )
                if (
                    measurement.valid
                    and measurement.confidence >= min_confidence
                ):
                    x_samples.append(measurement.global_x)
                    confidences.append(measurement.confidence)

                self.post_ui(
                    self._axis_update_progress,
                    i + 1, total_frames,
                    len(x_samples),
                )
                time.sleep(0.05)

            # 4. 异常值过滤
            if len(x_samples) < 3:
                self.post_ui(
                    self._axis_sampling_failed,
                    f"有效帧 {len(x_samples)}/{total_frames}，"
                    "低于最低要求 3 帧",
                )
                self.post_ui(self._axis_sampling_cleanup, "采样失败")
                return

            center1, mad1 = robust_sample(x_samples)
            threshold = max(3.0 * mad1, 1.5)
            filtered = [
                v for v in x_samples
                if abs(v - center1) <= threshold
            ]

            if len(filtered) < 3:
                self.post_ui(
                    self._axis_sampling_failed,
                    "异常值过滤后有效帧不足（<3）",
                )
                self.post_ui(self._axis_sampling_cleanup, "采样失败")
                return

            from statistics import median as stat_median

            final_x, final_mad = robust_sample(filtered)
            peak_to_peak = max(filtered) - min(filtered)
            median_conf = stat_median(confidences)

            # 5. 合格判断
            reject_reasons: list[str] = []
            valid_ratio = len(x_samples) / max(total_frames, 1)

            if valid_ratio < 0.70:
                reject_reasons.append(
                    f"有效比例 {len(x_samples)}/"
                    f"{total_frames} < 0.70"
                )
            if len(filtered) < 20:
                reject_reasons.append(
                    f"最终有效帧 {len(filtered)} < 20"
                )
            if final_mad > max_mad:
                reject_reasons.append(
                    f"MAD={final_mad:.2f} px > {max_mad}"
                )
            if peak_to_peak > 6.0:
                reject_reasons.append(
                    f"峰峰值={peak_to_peak:.2f} px > 6.0"
                )
            if median_conf < min_confidence:
                reject_reasons.append(
                    f"中位置信度={median_conf:.3f} "
                    f"< {min_confidence}"
                )

            if reject_reasons:
                self.post_ui(
                    self._axis_sampling_failed,
                    "采样失败：\n" + "\n".join(reject_reasons),
                )
                self.post_ui(self._axis_sampling_cleanup, "采样不合格")
                return

            # 6. 添加到标定点
            point = AxisCalibrationPoint(
                position_mm=position_mm,
                pixel_x=final_x,
                mad_px=final_mad,
                peak_to_peak_px=peak_to_peak,
                median_confidence=median_conf,
                valid_frames=len(filtered),
                total_frames=total_frames,
            )
            self.post_ui(self._axis_add_point, point)
            self.post_ui(
                self._axis_sampling_cleanup,
                f"采样完成：{position_mm} mm → "
                f"{final_x:.2f} px (MAD={final_mad:.2f})",
            )

        except Exception as exc:
            LOGGER.exception("采样线程异常")
            self.post_ui(
                self._axis_sampling_failed,
                f"采样异常：{exc}",
            )
            self.post_ui(self._axis_sampling_cleanup, "异常终止")

    def _axis_update_progress(
        self,
        current: int,
        total: int,
        valid: int,
    ) -> None:
        """主线程：更新采样进度。"""
        self.axis_status_var.set(
            f"采样进度：{current}/{total}"
            f"（有效检测 {valid}）"
        )

    def _axis_add_point(self, point: AxisCalibrationPoint) -> None:
        """主线程：添加或替换标定点。"""
        self.axis_calibration.replace_or_add(point)
        self.axis_dirty = True
        self._axis_refresh_table()
        self._axis_update_line_info()

    def _axis_sampling_failed(self, reason: str) -> None:
        """主线程：采样失败。"""
        messagebox.showwarning(
            "采样不合格",
            reason,
            parent=self.root,
        )
        self.axis_status_var.set("采样状态：不合格")

    def _axis_sampling_cleanup(self, msg: str = "") -> None:
        """主线程：采样结束收尾。"""
        self.axis_sampling = False
        self._set_axis_controls_enabled(True)
        if msg:
            self.axis_status_var.set(f"采样状态：{msg}")
            self.set_notice(msg)

    # ── 位置标定：表格管理 ──

    def _axis_refresh_table(self) -> None:
        """刷新标定点 Treeview。"""
        for row in self.axis_tree.get_children():
            self.axis_tree.delete(row)

        for i, point in enumerate(
            self.axis_calibration.sorted_points(),
            1,
        ):
            if point.is_historical:
                mad_str = "-"
                pp_str = "-"
                frames_str = "-"
                status = "历史数据"
            else:
                mad_str = f"{point.mad_px:.2f}"
                pp_str = f"{point.peak_to_peak_px:.2f}"
                frames_str = (
                    f"{point.valid_frames}/"
                    f"{point.total_frames}"
                )
                # 判断是否合格
                if (
                    point.valid_ratio >= 0.70
                    and point.valid_frames >= 20
                    and point.mad_px <= 1.5
                    and point.peak_to_peak_px <= 6.0
                    and point.median_confidence >= 0.55
                ):
                    status = "合格"
                else:
                    status = "不合格"

            self.axis_tree.insert(
                "",
                tk.END,
                values=(
                    i,
                    f"{point.position_mm:.3f}",
                    f"{point.pixel_x:.3f}",
                    mad_str,
                    pp_str,
                    frames_str,
                    status,
                ),
            )

    def _axis_get_selected_point(
        self,
    ) -> AxisCalibrationPoint | None:
        """返回当前选中行对应的标定点。"""
        selection = self.axis_tree.selection()
        if not selection:
            raise OperationError("请先在表格中选择一个标定点")

        values = self.axis_tree.item(selection[0], "values")
        try:
            position_mm = float(values[1])
        except (IndexError, ValueError):
            raise OperationError("无法解析选中行的位置")

        for point in self.axis_calibration.points:
            if abs(point.position_mm - position_mm) < 0.001:
                return point

        raise OperationError("找不到选中的标定点")

    @ui_guard("删除标定点")
    def _axis_delete_point(self) -> None:
        point = self._axis_get_selected_point()
        self.axis_calibration.points.remove(point)
        self.axis_dirty = True
        self._axis_refresh_table()
        self._axis_update_line_info()
        self.set_notice(
            f"已删除 {point.position_mm} mm 标定点"
        )

    @ui_guard("重新采样")
    def _axis_resample_point(self) -> None:
        """删除选中点并填入其物理位置，让用户重新采样。"""
        point = self._axis_get_selected_point()
        self.axis_position_var.set(f"{point.position_mm:.1f}")
        self.axis_calibration.points.remove(point)
        self.axis_dirty = True
        self._axis_refresh_table()
        self._axis_update_line_info()
        self.set_notice(
            f"已删除 {point.position_mm} mm 的旧数据，"
            "请点击「开始采样」重新采集"
        )

    @ui_guard("清空标定点")
    def _axis_clear_all(self) -> None:
        if not self.axis_calibration.points:
            return

        ok = messagebox.askyesno(
            "确认清空",
            "确定清空所有位置标定点吗？此操作不可恢复。",
            parent=self.root,
        )
        if not ok:
            return
        self.axis_calibration.points.clear()
        self.axis_dirty = True
        self._axis_refresh_table()
        self._axis_update_line_info()
        self.set_notice("已清空所有标定点")

    # ── 位置标定：保存 ──

    @ui_guard("保存位置标定")
    def _axis_save(self) -> None:
        """保存轴标定点到 vision.toml。"""
        if tomlkit is None:
            raise OperationError(
                "保存需要 tomlkit。"
                "运行：python3 -m pip install tomlkit"
            )

        calibration = self.axis_calibration
        if not calibration.points:
            raise OperationError("没有标定点可保存")

        rc = self._roi_calibration
        calibration.validate_for_save(full_roi=rc.full_roi)

        # 检查模式文件覆盖
        self._axis_check_mode_override()

        # 检查 0mm 点与启动线关系
        zero = calibration.find_zero_point()
        if zero is not None:
            line_error = abs(
                zero.pixel_x - rc.calibration_line_x
            )
            if line_error > rc.calibration_line_tolerance_px:
                raise OperationError(
                    f"0 mm 标定点不在任务启动线容差带内。\n"
                    f"0mm 点像素={zero.pixel_x:.1f}，"
                    f"启动线={rc.calibration_line_x}，\n"
                    f"偏差={line_error:.1f} px > "
                    f"容差={rc.calibration_line_tolerance_px} px。\n"
                    "请更新启动线，或重新采样 0 mm 点。"
                )

        points = calibration.sorted_points()
        pixels_text = ",".join(
            f"{p.pixel_x:.3f}" for p in points
        )
        positions_text = ",".join(
            f"{p.position_mm:.3f}" for p in points
        )

        vision_path = CONFIG_DIR / "vision.toml"
        text = vision_path.read_text(encoding="utf-8")
        document = tomlkit.parse(text)

        axis = (
            document.get("vision", {})
            .get("ball_ncnn", {})
            .get("axis_calibration")
        )
        if axis is None:
            raise OperationError(
                "vision.toml 中缺少 "
                "[vision.ball_ncnn.axis_calibration] 节"
            )

        axis["image_right_sign"] = calibration.image_right_sign
        axis["pixels"] = pixels_text
        axis["positions_mm"] = positions_text

        rendered = tomlkit.dumps(document)

        # 二次解析验证
        if tomllib is not None:
            try:
                tomllib.loads(rendered)
            except Exception as exc:
                raise OperationError(
                    f"保存后 TOML 解析失败：{exc}"
                ) from exc

        atomic_write_text(vision_path, rendered, make_backup=True)

        self.axis_dirty = False
        self.set_notice(
            "位置标定已保存。请重启主程序使新标定生效。"
        )
        LOGGER.info(
            "轴标定已保存：%d 个点",
            len(points),
        )

        # 重启 worker 使新配置生效
        if self.axis_worker is not None:
            self.axis_worker.restart()

        # 刷新配置页
        if self.current_config_rel == "vision.toml":
            self.load_config_file("vision.toml")

    def _axis_check_mode_override(self) -> None:
        """检查活动模式文件是否覆盖了标定参数。"""
        main_path = CONFIG_DIR / "main.toml"
        if not main_path.is_file() or tomllib is None:
            return

        with main_path.open("rb") as f:
            main_data = tomllib.load(f)

        mode_name = (
            main_data.get("mode", {}).get("name", "competition")
        )
        mode_path = CONFIG_DIR / "modes" / f"{mode_name}.toml"
        if not mode_path.is_file():
            return

        with mode_path.open("rb") as f:
            mode_data = tomllib.load(f)

        forbidden = (
            "axis_calibration",
            "calibration_line_x",
            "calibration_line_tolerance_px",

            "roi_location_mode",

            "full_roi_x",
            "full_roi_y",
            "full_src_width",
            "full_src_height",

            "center_roi_x",
            "center_roi_y",
            "center_src_width",
            "center_src_height",
        )

        def _contains_key(table, key: str) -> bool:
            """递归检查 table 及其嵌套表中是否包含指定键。"""
            if isinstance(table, dict):
                if key in table:
                    return True
                for value in table.values():
                    if _contains_key(value, key):
                        return True
            return False

        conflicts = [key for key in forbidden if _contains_key(mode_data, key)]

        if conflicts:
            raise OperationError(
                f"当前模式文件 ({mode_name}.toml) "
                "覆盖了标定参数："
                + ", ".join(conflicts)
                + "\n请先删除模式文件中的这些字段。"
            )

    # ── 位置标定：0mm 点与启动线同步 ──

    def _axis_update_line_info(self) -> None:
        """更新 0mm 点与启动线关系显示。"""
        zero = self.axis_calibration.find_zero_point()
        if zero is None:
            self.axis_line_info_var.set(
                "0 mm 标定点像素：-  "
                "任务启动线 X：-  偏差：-"
            )
            return

        line_x = self._roi_calibration.calibration_line_x
        deviation = zero.pixel_x - line_x
        self.axis_line_info_var.set(
            f"0 mm 标定点像素：{zero.pixel_x:.2f}  "
            f"任务启动线 X：{line_x}  "
            f"偏差：{deviation:+.2f} px"
        )

    @ui_guard("更新启动线")
    def _axis_sync_line_to_zero(self) -> None:
        """把任务启动线 X 设置为 0mm 标定点的像素值。"""
        zero = self.axis_calibration.find_zero_point()
        if zero is None:
            raise OperationError(
                "当前标定表没有 0 mm 点。"
                "请先采样 0 mm 位置。"
            )

        rc = self._roi_calibration
        rc.calibration_line_x = round(zero.pixel_x)
        rc.clamp_all()
        self.roi_dirty = True
        self._calib_sync_vars_from_model()
        self._calib_redraw()
        self._axis_update_line_info()
        self.set_notice(
            f"任务启动线已更新为 {rc.calibration_line_x} "
            f"（来自 0 mm 点 {zero.pixel_x:.2f} px）"
        )

    # ── 位置标定：验证模式 ──

    @ui_guard("验证位置")
    def _axis_verify_position(self) -> None:
        """验证模式：输入已知位置 → 采样 → 用新标定表换算 → 显示误差。"""
        if not self.axis_calibration.points:
            raise OperationError("请先完成至少 3 个标定点采样")

        self._axis_check_prerequisites()

        # 弹窗输入已知位置
        dialog = tk.Toplevel(self.root)
        dialog.title("验证位置")
        dialog.geometry("380x180")
        dialog.transient(self.root)
        dialog.grab_set()

        ttk.Label(
            dialog, text="输入已知物理位置 (mm)：", font=("", 10),
        ).pack(pady=(16, 4))

        verify_var = tk.StringVar(value="25.0")
        ttk.Entry(
            dialog, textvariable=verify_var, width=14, font=("", 12),
        ).pack(pady=(0, 12))

        result_var = tk.StringVar(value="")

        def do_verify():
            try:
                known_mm = float(verify_var.get())
            except ValueError:
                messagebox.showerror(
                    "输入错误", "请输入有效数字", parent=dialog,
                )
                return

            # 确保 worker 运行
            if self.axis_worker is None:
                self._axis_start_worker()
            self.axis_worker.reset_tracking()

            # 丢弃 + 采集 20 帧
            for _ in range(3):
                frame = self.calib_camera.latest_frame()
                if frame is not None:
                    self.axis_worker.infer(
                        frame, mode="FULL", timeout_s=2.0,
                    )

            x_samples: list[float] = []
            for _ in range(20):
                frame = self.calib_camera.latest_frame()
                if frame is None:
                    continue
                m = self.axis_worker.infer(
                    frame, mode="FULL", timeout_s=2.0,
                )
                if m.valid and m.confidence >= 0.55:
                    x_samples.append(m.global_x)
                time.sleep(0.05)

            if len(x_samples) < 5:
                result_var.set("有效帧不足，无法验证")
                return

            center_x, _ = robust_sample(x_samples)
            predicted_mm = self.axis_calibration.pixel_to_mm(
                center_x
            )
            error_mm = predicted_mm - known_mm

            result_var.set(
                f"已知：{known_mm:.3f} mm\n"
                f"检测像素：{center_x:.2f} px\n"
                f"预测：{predicted_mm:.3f} mm\n"
                f"误差：{error_mm:+.3f} mm"
            )

        ttk.Button(
            dialog, text="开始验证", command=do_verify,
        ).pack(pady=(0, 8))

        ttk.Label(
            dialog, textvariable=result_var,
            justify=tk.LEFT, font=("", 10),
        ).pack()

    # ── 位置标定：辅助 ──

    def _axis_update_cam_status(self) -> None:
        """定时更新摄像头状态显示。"""
        if self.closed:
            return
        if self.calib_running:
            self.axis_cam_status_var.set("摄像头：运行中")
        else:
            self.axis_cam_status_var.set("摄像头：未启动")
        self.root.after(2000, self._axis_update_cam_status)

    def _set_axis_controls_enabled(self, enabled: bool) -> None:
        """启用/禁用位置标定页的所有交互控件。"""
        state = tk.NORMAL if enabled else tk.DISABLED
        for widget in self._axis_control_widgets:
            try:
                widget.configure(state=state)
            except tk.TclError:
                pass

    # ── 辅助 ──

    def set_notice(self, text: str, error: bool = False) -> None:
        self.notice_var.set(text)
        if error:
            LOGGER.error("界面提示：%s", text)

    @ui_guard("关闭 GUI")
    def on_close(self) -> None:
        if self.config_dirty:
            close_anyway = messagebox.askyesno(
                "存在未保存修改",
                "配置有未保存修改，确定关闭 GUI 吗？\n"
                "主程序不会因为关闭 GUI 而停止。",
                parent=self.root,
            )
            if not close_anyway:
                return

        self.closed = True

        # 停止标定摄像头定时器
        if getattr(self, "calib_after_id", None) is not None:
            try:
                self.root.after_cancel(self.calib_after_id)
            except tk.TclError:
                pass
            self.calib_after_id = None

        # 停止标定摄像头线程
        if getattr(self, "calib_camera", None) is not None:
            self.calib_camera.stop()
            self.calib_camera = None

        self.calib_running = False

        if self.process_console_handle is not None:
            try:
                self.process_console_handle.close()
            except OSError:
                LOGGER.warning("关闭控制台日志句柄失败", exc_info=True)
            self.process_console_handle = None

        LOGGER.info("GUI 关闭；不停止主程序")
        self.root.destroy()


def main() -> int:
    try:
        LOG_DIR.mkdir(parents=True, exist_ok=True)
        RUN_DIR.mkdir(parents=True, exist_ok=True)

        root = tk.Tk()
        EtestGui(root)
        root.mainloop()
        return 0
    except Exception:
        LOGGER.exception("GUI 启动失败")
        try:
            messagebox.showerror(
                "GUI 启动失败",
                f"详细错误已记录到：\n{GUI_LOG_FILE}",
            )
        except Exception:
            pass
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
