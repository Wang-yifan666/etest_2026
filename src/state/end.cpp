#include "state/end.hpp"

#include "core/logger.hpp"

namespace etest::state
{

	State runEnd(AppContext& ctx)
	{
		ETEST_LOG_INFO("END", "Mission complete, shutting down ...");

		ctx.running = false;

		return State::END;
	}

} // namespace etest::state
