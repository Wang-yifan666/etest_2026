#pragma once

#include "core/config.hpp"
#include "core/context.hpp"
#include "state/state.hpp"

namespace etest::state
{

	State runSearch(AppContext& ctx, const SearchConfig& search_cfg);

}
