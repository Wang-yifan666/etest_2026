#include "state/calibration_logic.hpp"

#include <algorithm>
#include <cmath>

namespace etest
{
	namespace state
	{

		bool isBallOnCalibrationLine(const float global_x,
		                             const float calibration_line_x,
		                             const float tolerance_px) noexcept
		{
			return std::abs(global_x - calibration_line_x)
			    <= tolerance_px;
		}

		CalibrationGateResult updateCalibrationGate(
		    CalibrationGateState& state, const float global_x,
		    const float calibration_line_x, const int required_frames,
		    const float max_jitter_px, const bool on_line) noexcept
		{
			CalibrationGateResult result;

			result.on_line = on_line;
			result.line_error_px = global_x - calibration_line_x;

			if(!on_line)
			{
				// 不满足线上条件：重置
				state.valid_frames = 0;
				state.x_samples.clear();
				return result;
			}

			// 添加样本
			state.x_samples.push_back(global_x);

			while(static_cast<int>(state.x_samples.size())
			      > required_frames)
			{
				state.x_samples.pop_front();
			}

			// 样本数不够，还不能判断
			if(static_cast<int>(state.x_samples.size())
			   < required_frames)
			{
				return result;
			}

			// 计算窗口抖动
			const auto [min_it, max_it] = std::minmax_element(
			    state.x_samples.begin(), state.x_samples.end());

			const float jitter = *max_it - *min_it;

			result.jitter_px = jitter;

			if(jitter <= max_jitter_px)
			{
				++state.valid_frames;

				if(state.valid_frames >= required_frames)
				{
					result.ready = true;
				}
			}
			else
			{
				// 抖动过大：清零
				state.valid_frames = 0;
			}

			return result;
		}

	} // namespace state
} // namespace etest