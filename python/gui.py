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
    RoiCalibration,
    RoiRect,
    AxisCalibration,
    AxisCalibrationPoint,
    clamp,
    robust_sample,
)
from calibration_camera import CalibrationCamera  # type: ignore[import-untyped]

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

        ttk.Label(toolbar, text="拖动模式：").pack(side=tk.LEFT)
        self.calib_mode_var = tk.StringVar(value="full_roi")
        calib_mode_box = ttk.Combobox(
            toolbar,
            textvariable=self.calib_mode_var,
            values=("full_roi", "center_roi", "center_line"),
            state="readonly",
            width=14,
        )
        calib_mode_box.pack(side=tk.LEFT, padx=(4, 8))
        calib_mode_box.bind("<<ComboboxSelected>>", self._calib_on_mode_changed)

        ttk.Button(
            toolbar, text="打开摄像头", command=self._calib_start_camera
        ).pack(side=tk.LEFT, padx=3)

        ttk.Button(
            toolbar, text="停止摄像头", command=self._calib_stop_camera
        ).pack(side=tk.LEFT, padx=3)

        ttk.Button(
            toolbar, text="重新加载配置", command=self._calib_reload
        ).pack(side=tk.LEFT, padx=3)

        ttk.Button(
            toolbar, text="保存标定", command=self._calib_save
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
        input_frame = ttk.LabelFrame(main_frame, text="参数", padding=8)
        input_frame.pack(side=tk.RIGHT, fill=tk.Y, padx=(8, 0))

        # Full ROI
        ttk.Label(input_frame, text="Full ROI", font=("", 10, "bold")).grid(
            row=0, column=0, columnspan=2, sticky=tk.W, pady=(0, 4)
        )
        ttk.Label(input_frame, text="X:").grid(row=1, column=0, sticky=tk.E, padx=(0, 4))
        self.full_roi_x_var = tk.IntVar()
        ttk.Entry(input_frame, textvariable=self.full_roi_x_var, width=6).grid(
            row=1, column=1, sticky=tk.W
        )
        self.full_roi_x_var.trace_add("write", lambda *_: self._calib_apply_entry_values())

        ttk.Label(input_frame, text="Y:").grid(row=2, column=0, sticky=tk.E, padx=(0, 4))
        self.full_roi_y_var = tk.IntVar()
        ttk.Entry(input_frame, textvariable=self.full_roi_y_var, width=6).grid(
            row=2, column=1, sticky=tk.W
        )
        self.full_roi_y_var.trace_add("write", lambda *_: self._calib_apply_entry_values())

        ttk.Label(input_frame, text="W: 1280 (只读)").grid(
            row=3, column=0, columnspan=2, sticky=tk.W, pady=(0, 2)
        )
        ttk.Label(input_frame, text="H: 320 (只读)").grid(
            row=4, column=0, columnspan=2, sticky=tk.W, pady=(0, 8)
        )

        # Center ROI
        ttk.Label(input_frame, text="Center ROI", font=("", 10, "bold")).grid(
            row=5, column=0, columnspan=2, sticky=tk.W, pady=(0, 4)
        )
        ttk.Label(input_frame, text="X:").grid(row=6, column=0, sticky=tk.E, padx=(0, 4))
        self.center_roi_x_var = tk.IntVar()
        ttk.Entry(input_frame, textvariable=self.center_roi_x_var, width=6).grid(
            row=6, column=1, sticky=tk.W
        )
        self.center_roi_x_var.trace_add("write", lambda *_: self._calib_apply_entry_values())

        ttk.Label(input_frame, text="Y:").grid(row=7, column=0, sticky=tk.E, padx=(0, 4))
        self.center_roi_y_var = tk.IntVar()
        ttk.Entry(input_frame, textvariable=self.center_roi_y_var, width=6).grid(
            row=7, column=1, sticky=tk.W
        )
        self.center_roi_y_var.trace_add("write", lambda *_: self._calib_apply_entry_values())

        ttk.Label(input_frame, text="W: 448 (只读)").grid(
            row=8, column=0, columnspan=2, sticky=tk.W, pady=(0, 2)
        )
        ttk.Label(input_frame, text="H: 320 (只读)").grid(
            row=9, column=0, columnspan=2, sticky=tk.W, pady=(0, 8)
        )

        # 水平参考线
        ttk.Label(input_frame, text="水平参考线", font=("", 10, "bold")).grid(
            row=10, column=0, columnspan=2, sticky=tk.W, pady=(0, 4)
        )
        ttk.Label(input_frame, text="Y:").grid(row=11, column=0, sticky=tk.E, padx=(0, 4))
        self.center_line_y_var = tk.IntVar()
        ttk.Entry(input_frame, textvariable=self.center_line_y_var, width=6).grid(
            row=11, column=1, sticky=tk.W
        )
        self.center_line_y_var.trace_add("write", lambda *_: self._calib_apply_entry_values())

        ttk.Label(
            input_frame,
            text="仅用于画面辅助观察\n不参与位置换算",
            foreground="#888888",
            font=("", 8),
        ).grid(row=12, column=0, columnspan=2, sticky=tk.W, pady=(2, 0))

        # ── 数据模型 ──
        self._roi_calibration = RoiCalibration()
        self._calib_load_from_config()

        # 同步到输入框
        self._calib_sync_vars_from_model()

        # ── 摄像头 ──
        self.calib_camera: Optional[CalibrationCamera] = None
        self.calib_running = False
        self.calib_current_frame = None
        self.calib_after_id: Optional[str] = None

        # ── 拖动状态 ──
        self.calib_drag_item: Optional[str] = None
        self.calib_drag_offset_x = 0.0
        self.calib_drag_offset_y = 0.0
        self.calib_transform: Optional[CanvasTransform] = None

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

    # ── 数据模型 ⇄ 输入框 ──

    def _calib_sync_vars_from_model(self) -> None:
        """把数据模型同步到 IntVar 输入框。"""
        rc = self._roi_calibration
        self.full_roi_x_var.set(rc.full_roi.x)
        self.full_roi_y_var.set(rc.full_roi.y)
        self.center_roi_x_var.set(rc.center_roi.x)
        self.center_roi_y_var.set(rc.center_roi.y)
        self.center_line_y_var.set(rc.center_line_y)

    def _calib_apply_entry_values(self) -> None:
        """把 IntVar 值应用到数据模型，并执行边界钳制。"""
        rc = self._roi_calibration
        try:
            rc.full_roi.x = self.full_roi_x_var.get()
        except tk.TclError:
            pass

        try:
            rc.full_roi.y = self.full_roi_y_var.get()
        except tk.TclError:
            pass

        try:
            rc.center_roi.x = self.center_roi_x_var.get()
        except tk.TclError:
            pass

        try:
            rc.center_roi.y = self.center_roi_y_var.get()
        except tk.TclError:
            pass

        try:
            rc.center_line_y = self.center_line_y_var.get()
        except tk.TclError:
            pass

        rc.clamp_all()
        self._calib_sync_vars_from_model()
        self._calib_redraw()

    def _calib_nudge(self, dx: int, dy: int, event: tk.Event) -> None:
        """方向键微调当前模式的 ROI/参考线位置。"""
        step = 10 if (event.state & 0x0001) else 1  # Shift 键
        mode = self.calib_mode_var.get()
        rc = self._roi_calibration

        if mode == "full_roi":
            rc.full_roi.x += dx * step
            rc.full_roi.y += dy * step
        elif mode == "center_roi":
            rc.center_roi.x += dx * step
            rc.center_roi.y += dy * step
        elif mode == "center_line":
            rc.center_line_y += dy * step

        rc.clamp_all()
        self._calib_sync_vars_from_model()
        self._calib_redraw()

    # ── 配置读写 ──

    def _calib_load_from_config(self) -> None:
        """从 config/vision.toml 读取当前标定值。"""
        vision_path = CONFIG_DIR / "vision.toml"
        try:
            if vision_path.is_file():
                with vision_path.open("rb") as f:
                    data = tomllib.load(f) if tomllib else {}
                bn = data.get("vision", {}).get("ball_ncnn", {})
                rc = self._roi_calibration
                rc.full_roi.x = int(bn.get("full_roi_x", 0))
                rc.full_roi.y = int(bn.get("full_roi_y", 160))
                rc.center_roi.x = int(bn.get("center_roi_x", 416))
                rc.center_roi.y = int(bn.get("center_roi_y", 160))
                rc.center_line_y = int(bn.get("center_line_y", 320))
                rc.clamp_all()
                LOGGER.info("标定参数已从配置加载")
        except Exception:
            LOGGER.warning("从配置加载标定参数失败，使用默认值", exc_info=True)

    @ui_guard("重新加载标定配置")
    def _calib_reload(self) -> None:
        self._calib_load_from_config()
        self._calib_sync_vars_from_model()
        self.calib_info_var.set("已从配置重新加载标定参数")
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

        ball_ncnn["roi_location_mode"] = "topleft"
        ball_ncnn["full_roi_x"] = rc.full_roi.x
        ball_ncnn["full_roi_y"] = rc.full_roi.y
        ball_ncnn["center_roi_x"] = rc.center_roi.x
        ball_ncnn["center_roi_y"] = rc.center_roi.y
        ball_ncnn["center_line_y"] = rc.center_line_y

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
            "标定参数已保存: full=(%d,%d) center=(%d,%d) line_y=%d",
            rc.full_roi.x, rc.full_roi.y,
            rc.center_roi.x, rc.center_roi.y,
            rc.center_line_y,
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

        self.calib_camera = CalibrationCamera(0)
        self.calib_camera.start()
        self.calib_running = True
        self.calib_info_var.set("摄像头已启动 — 拖动/方向键/输入框调整参数")
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
        self.calib_info_var.set("摄像头已停止")

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
        GREEN_LIGHT = "#55ff55"
        mode = self.calib_mode_var.get()
        rc = self._roi_calibration

        # Full ROI 矩形
        fx, fy = transform.image_to_canvas(rc.full_roi.x, rc.full_roi.y)
        fx2, fy2 = transform.image_to_canvas(
            rc.full_roi.x + rc.full_roi.width,
            rc.full_roi.y + rc.full_roi.height,
        )
        self.calib_canvas.create_rectangle(
            fx, fy, fx2, fy2,
            outline=GREEN, width=2, tags="full_roi",
        )

        # Center ROI 矩形
        cx, cy = transform.image_to_canvas(rc.center_roi.x, rc.center_roi.y)
        cx2, cy2 = transform.image_to_canvas(
            rc.center_roi.x + rc.center_roi.width,
            rc.center_roi.y + rc.center_roi.height,
        )
        self.calib_canvas.create_rectangle(
            cx, cy, cx2, cy2,
            outline=GREEN_LIGHT, width=2, dash=(6, 3), tags="center_roi",
        )

        # 水平参考线（仅在图像区域内绘制）
        line_left, line_y = transform.image_to_canvas(0, rc.center_line_y)
        line_right, _ = transform.image_to_canvas(rc.frame_width, rc.center_line_y)
        self.calib_canvas.create_line(
            line_left, line_y, line_right, line_y,
            fill=GREEN, width=3, tags="center_line",
        )

        # 拖动句柄
        if mode == "full_roi":
            self.calib_canvas.create_rectangle(
                fx - 4, fy - 4, fx + 4, fy + 4,
                fill=GREEN, outline=GREEN, tags="handle_f",
            )
        elif mode == "center_roi":
            self.calib_canvas.create_rectangle(
                cx - 4, cy - 4, cx + 4, cy + 4,
                fill=GREEN_LIGHT, outline=GREEN_LIGHT, tags="handle_c",
            )
        elif mode == "center_line":
            self.calib_canvas.create_line(
                line_left, line_y, line_right, line_y,
                fill=GREEN, width=5, tags="handle_l",
            )

    def _calib_redraw(self) -> None:
        """仅重绘叠加层（不清空画面）。"""
        self.calib_canvas.delete(
            "full_roi", "center_roi", "center_line",
            "handle_f", "handle_c", "handle_l",
        )
        if self.calib_current_frame is not None:
            self._calib_draw_overlay()

    # ── 鼠标事件 ──

    @ui_guard("拖动模式切换")
    def _calib_on_mode_changed(self, _event=None) -> None:
        self._calib_redraw()
        mode = self.calib_mode_var.get()
        mode_names = {
            "full_roi": "Full ROI",
            "center_roi": "Center ROI",
            "center_line": "水平参考线",
        }
        self.calib_info_var.set(
            f"当前拖动：{mode_names.get(mode, mode)}"
        )

    def _calib_on_press(self, event: tk.Event) -> None:
        transform = self.calib_transform
        if transform is None:
            return

        if not transform.contains_canvas_point(event.x, event.y):
            self.set_notice("鼠标位置不在有效图像区域", error=True)
            return

        image_x, image_y = transform.canvas_to_image(event.x, event.y)
        mode = self.calib_mode_var.get()
        rc = self._roi_calibration

        if mode == "full_roi":
            roi = rc.full_roi
            # 近似的句柄检测：鼠标在 ROI 左上角 12 CV-px 以内
            handle_cx, handle_cy = transform.image_to_canvas(roi.x, roi.y)
            if abs(event.x - handle_cx) < 12 and abs(event.y - handle_cy) < 12:
                self.calib_drag_item = "full_roi"
                self.calib_drag_offset_x = image_x - roi.x
                self.calib_drag_offset_y = image_y - roi.y

        elif mode == "center_roi":
            roi = rc.center_roi
            handle_cx, handle_cy = transform.image_to_canvas(roi.x, roi.y)
            if abs(event.x - handle_cx) < 12 and abs(event.y - handle_cy) < 12:
                self.calib_drag_item = "center_roi"
                self.calib_drag_offset_x = image_x - roi.x
                self.calib_drag_offset_y = image_y - roi.y

        elif mode == "center_line":
            self.calib_drag_item = "center_line"
            self.calib_drag_offset_y = image_y - rc.center_line_y

    def _calib_on_drag(self, event: tk.Event) -> None:
        if self.calib_drag_item is None or self.calib_transform is None:
            return

        transform = self.calib_transform
        if not transform.contains_canvas_point(event.x, event.y):
            return

        image_x, image_y = transform.canvas_to_image(event.x, event.y)
        rc = self._roi_calibration

        if self.calib_drag_item == "full_roi":
            rc.full_roi.x = round(image_x - self.calib_drag_offset_x)
            rc.full_roi.y = round(image_y - self.calib_drag_offset_y)
        elif self.calib_drag_item == "center_roi":
            rc.center_roi.x = round(image_x - self.calib_drag_offset_x)
            rc.center_roi.y = round(image_y - self.calib_drag_offset_y)
        elif self.calib_drag_item == "center_line":
            rc.center_line_y = round(image_y - self.calib_drag_offset_y)

        rc.clamp_all()
        self._calib_sync_vars_from_model()
        self._calib_redraw()
        self._calib_update_info()

    def _calib_on_release(self, _event: tk.Event) -> None:
        self.calib_drag_item = None

    def _calib_on_resize(self, _event: tk.Event) -> None:
        if self.calib_current_frame is not None:
            self._calib_render_frame(self.calib_current_frame)

    def _calib_update_info(self) -> None:
        mode = self.calib_mode_var.get()
        rc = self._roi_calibration
        if mode == "full_roi":
            self.calib_info_var.set(
                f"Full ROI 左上角: ({rc.full_roi.x}, {rc.full_roi.y})"
            )
        elif mode == "center_roi":
            self.calib_info_var.set(
                f"Center ROI 左上角: ({rc.center_roi.x}, {rc.center_roi.y})"
            )
        elif mode == "center_line":
            self.calib_info_var.set(
                f"水平参考线 Y: {rc.center_line_y}"
            )

    # ── 位置标定子页面（第二阶段占位）──

    def _build_axis_tab(self) -> None:
        placeholder = ttk.Label(
            self.calib_axis_tab,
            text="位置标定功能将在第二阶段实现。\n\n"
                 "功能：\n"
                 "• 输入已知物理位置（如 -100 mm）\n"
                 "• 调用 NCNN 检测器多帧采样球心像素\n"
                 "• 计算中位数 / MAD 抖动 / 有效帧数\n"
                 "• 标定点表格（可添加/删除/重采样）\n"
                 "• 保存 pixels 与 positions_mm 到 vision.toml",
            justify=tk.LEFT,
            font=("", 11),
        )
        placeholder.pack(expand=True)

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
