#include "state/start.hpp"

#include <iostream>

namespace etest::state
{

	State runStart([[maybe_unused]] AppContext& ctx)
	{
		std::cout << "[START] System initializing ...\n";

		// TODO: 硬件自检、初始化校准等

		std::cout << "[START] System ready, entering SEARCH state\n";

		return State::SEARCH;
	}

} // namespace etest::state
