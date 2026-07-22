#include "state/search.hpp"

#include <iostream>

namespace etest::state
{

	State runSearch([[maybe_unused]] AppContext& ctx)
	{
		std::cout << "[SEARCH] Searching target...\n";

		// TODO: 调用视觉模块检测目标
		// cv::Mat frame;
		// if(ctx.camera.read(frame))
		// {
		//     auto res = ctx.vision.process(frame, vision::VisionMode::ColorTarget);
		// }

		return State::SEARCH;
	}

} // namespace etest::state
