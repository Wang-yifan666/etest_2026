#include "state/end.hpp"

#include "core/logger.hpp"

namespace etest::state
{

	State runEnd(AppContext& ctx)
	{
		ETEST_LOG_INFO("END", "shutting down");

		ctx.running = false;

		// 如果最后一次故障是 RESTART_PROCESS，记录
		if(ctx.last_fault.source != FaultSource::NONE)
		{
			ETEST_LOG_INFO("END",
			               "last fault: source="
			                   + std::to_string(static_cast<int>(
			                       ctx.last_fault.source))
			                   + ", message=" + ctx.last_fault.message);
		}

		return State::END;
	}

} // namespace etest::state