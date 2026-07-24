#include "state/start.hpp"

#include "core/logger.hpp"

namespace etest::state
{

	State runStart([[maybe_unused]] AppContext& ctx)
	{
		ETEST_LOG_INFO("STATE_START", "system initialization started");

		// TODO:
		// 1. 串口自检；
		// 2. 摄像头参数确认；
		// 3. 控制板握手；
		// 4. 必要的初始标定。

		ETEST_LOG_INFO("STATE_START",
		               "system ready; entering SEARCH state");

		return State::SEARCH;
	}

} // namespace etest::state
