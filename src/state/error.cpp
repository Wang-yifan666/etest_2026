#include "state/error.hpp"

#include <iostream>

namespace etest::state
{

	State runError([[maybe_unused]] AppContext& ctx)
	{
		std::cerr << "[ERROR] System error occurred\n";

		return State::END;
	}

} // namespace etest::state
