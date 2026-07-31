#pragma once

#include <deque>
#include <cstddef>

namespace etest
{
namespace state
{
	/// 任务启动标定门控状态。
	/// Q3～Q5 使用竖线判断；Q6 使用任意位置稳定性判断。
	struct CalibrationGateState
	{
		int valid_frames = 0;
		std::deque<float> x_samples;
	};

	/// 门控判决结果。
	struct CalibrationGateResult
	{
		bool ready = false;
		bool on_line = false;

		float line_error_px = 0.0F;
		float jitter_px = 0.0F;
	};

	/// 球是否位于标定线容差带内。
	/// 只比较 X 坐标，Y 不受限制。
	bool isBallOnCalibrationLine(
	    float global_x,
	    float calibration_line_x,
	    float tolerance_px) noexcept;

	/// 更新门控状态窗口。
	///
	/// @param on_line  当前帧是否满足线上条件。
	///                 Q3～Q5：调用者先判断竖线；
	///                 Q6：传 true（不需要竖线判断）。
	///
	/// @return 判定结果。ready=true 表示满足 N 帧连续稳定条件。
	CalibrationGateResult updateCalibrationGate(
	    CalibrationGateState& state,
	    float global_x,
	    float calibration_line_x,
	    int required_frames,
	    float max_jitter_px,
	    bool on_line) noexcept;

} // namespace state
} // namespace etest