#include "state/error.hpp"

#include "core/logger.hpp"

namespace etest::state
{

	State runError([[maybe_unused]] AppContext& ctx)
	{
		ETEST_LOG_ERROR("STATE_ERROR", "System entered ERROR state");

		return State::END;
	}

} // namespace etest::state
