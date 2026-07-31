"""
标定数据模型（ROI 配置 + 轴标定点 + Canvas 坐标映射）。

从 gui.py 中抽出，避免标定逻辑继续膨胀在主界面类中。
"""

from __future__ import annotations

from dataclasses import dataclass, field
from statistics import median
from typing import Iterable


class CalibrationValidationError(ValueError):
    """标定数据验证失败。"""


def clamp(value: int, minimum: int, maximum: int) -> int:
    return max(minimum, min(value, maximum))


# ── ROI ──

@dataclass
class RoiRect:
    x: int
    y: int
    width: int
    height: int

    def clamp_to_frame(self, frame_width: int, frame_height: int) -> None:
        if self.width <= 0 or self.height <= 0:
            raise CalibrationValidationError("ROI 宽高必须大于 0")

        if self.width > frame_width:
            raise CalibrationValidationError(
                f"ROI 宽度 {self.width} 超过画面宽度 {frame_width}"
            )

        if self.height > frame_height:
            raise CalibrationValidationError(
                f"ROI 高度 {self.height} 超过画面高度 {frame_height}"
            )

        self.x = clamp(self.x, 0, frame_width - self.width)
        self.y = clamp(self.y, 0, frame_height - self.height)

    def validate(self, frame_width: int, frame_height: int) -> None:
        if self.width <= 0 or self.height <= 0:
            raise CalibrationValidationError("ROI 宽高必须大于 0")

        if self.x < 0 or self.y < 0:
            raise CalibrationValidationError("ROI 坐标不能为负数")

        if self.x + self.width > frame_width:
            raise CalibrationValidationError("ROI 超出画面右边界")

        if self.y + self.height > frame_height:
            raise CalibrationValidationError("ROI 超出画面下边界")


@dataclass
class RoiCalibration:
    frame_width: int = 1280
    frame_height: int = 640

    full_roi: RoiRect = field(
        default_factory=lambda: RoiRect(0, 160, 1280, 320)
    )

    center_roi: RoiRect = field(
        default_factory=lambda: RoiRect(416, 160, 448, 320)
    )

    center_line_y: int = 320

    def clamp_all(self) -> None:
        self.full_roi.clamp_to_frame(
            self.frame_width,
            self.frame_height,
        )

        self.center_roi.clamp_to_frame(
            self.frame_width,
            self.frame_height,
        )

        self.center_line_y = clamp(
            self.center_line_y,
            0,
            self.frame_height - 1,
        )

    def validate(self) -> None:
        self.full_roi.validate(
            self.frame_width,
            self.frame_height,
        )

        self.center_roi.validate(
            self.frame_width,
            self.frame_height,
        )

        if not 0 <= self.center_line_y < self.frame_height:
            raise CalibrationValidationError(
                f"中心线必须位于 0~{self.frame_height - 1}"
            )


# ── 轴标定 ──

@dataclass
class AxisCalibrationPoint:
    position_mm: float
    pixel_x: float
    jitter_px: float = 0.0
    valid_frames: int = 1


@dataclass
class AxisCalibration:
    points: list[AxisCalibrationPoint] = field(default_factory=list)
    image_right_sign: int = 1

    def sorted_points(self) -> list[AxisCalibrationPoint]:
        return sorted(self.points, key=lambda point: point.pixel_x)

    def validate(self) -> None:
        if self.image_right_sign not in (-1, 1):
            raise CalibrationValidationError(
                "image_right_sign 只能是 -1 或 1"
            )

        points = self.sorted_points()

        if len(points) < 2:
            raise CalibrationValidationError("至少需要两个位置标定点")

        for previous, current in zip(points, points[1:]):
            if current.pixel_x <= previous.pixel_x:
                raise CalibrationValidationError(
                    "标定点的像素 X 必须严格递增，不能重复"
                )

        positions = [point.position_mm for point in points]

        increasing = all(
            current > previous
            for previous, current in zip(positions, positions[1:])
        )

        decreasing = all(
            current < previous
            for previous, current in zip(positions, positions[1:])
        )

        if not increasing and not decreasing:
            raise CalibrationValidationError(
                "物理位置必须随像素 X 单调变化"
            )

    def pixel_to_mm(self, pixel_x: float) -> float:
        """
        分段线性插值：全局像素 x → 毫米。

        超出标定范围时钳制到端点（与当前 C++ 行为一致）。
        """
        self.validate()
        points = self.sorted_points()

        if pixel_x <= points[0].pixel_x:
            return points[0].position_mm * self.image_right_sign

        if pixel_x >= points[-1].pixel_x:
            return points[-1].position_mm * self.image_right_sign

        for left, right in zip(points, points[1:]):
            if pixel_x <= right.pixel_x:
                span = right.pixel_x - left.pixel_x

                ratio = (pixel_x - left.pixel_x) / span

                value = (
                    left.position_mm
                    + ratio * (right.position_mm - left.position_mm)
                )

                return value * self.image_right_sign

        raise RuntimeError("无法完成像素坐标插值")


# ── 统计 ──

def robust_sample(values: Iterable[float]) -> tuple[float, float]:
    """
    返回 (中位数, 中位数绝对偏差 MAD)。

    不使用平均值，因为偶发的错误检测框会明显拉偏平均值；
    中位数和 MAD 对异常值更稳。
    """
    samples = list(values)

    if not samples:
        raise CalibrationValidationError("没有有效采样")

    center = median(samples)
    deviations = sorted(abs(value - center) for value in samples)
    mad = median(deviations)

    return center, mad


# ── Canvas 坐标映射 ──

@dataclass
class CanvasTransform:
    """
    统一的 Canvas ↔ 图像坐标转换。

    画面按宽高比 fit 进 Canvas，居中放置，两侧留黑边。
    所有绘制和鼠标事件都必须经过这个类，确保黑边区域不会被误当作有效图像区域。
    """
    frame_width: int
    frame_height: int
    canvas_width: int
    canvas_height: int

    def __post_init__(self) -> None:
        self.scale = min(
            self.canvas_width / max(self.frame_width, 1),
            self.canvas_height / max(self.frame_height, 1),
        )

        self.draw_width = max(
            1,
            round(self.frame_width * self.scale),
        )

        self.draw_height = max(
            1,
            round(self.frame_height * self.scale),
        )

        self.offset_x = (
            self.canvas_width - self.draw_width
        ) // 2

        self.offset_y = (
            self.canvas_height - self.draw_height
        ) // 2

    def image_to_canvas(
        self,
        image_x: float,
        image_y: float,
    ) -> tuple[float, float]:
        return (
            self.offset_x + image_x * self.scale,
            self.offset_y + image_y * self.scale,
        )

    def canvas_to_image(
        self,
        canvas_x: float,
        canvas_y: float,
    ) -> tuple[float, float]:
        return (
            (canvas_x - self.offset_x) / self.scale,
            (canvas_y - self.offset_y) / self.scale,
        )

    def contains_canvas_point(
        self,
        canvas_x: float,
        canvas_y: float,
    ) -> bool:
        return (
            self.offset_x
            <= canvas_x
            < self.offset_x + self.draw_width
            and self.offset_y
            <= canvas_y
            < self.offset_y + self.draw_height
        )