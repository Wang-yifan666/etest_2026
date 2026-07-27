#pragma once

#include "core/config.hpp"
#include "core/context.hpp"
#include "state/state.hpp"

namespace etest::state
{

State runStart(AppContext& ctx, const RuntimeConfig& runtime,
               const SearchConfig& search_cfg,
               bool show_preview);

}