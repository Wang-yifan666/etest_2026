#include "state/end.hpp"

#include <iostream>

namespace etest::state
{

	State runEnd(AppContext& ctx)
	{
		std::cout << "[END] Mission complete, shutting down ...\n";

		ctx.running = false;

		return State::END;
	}

} // namespace etest::state
