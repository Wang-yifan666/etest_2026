#include "core/context.hpp"

#include "state/error.hpp"
#include "state/search.hpp"
#include "state/start.hpp"
#include "state/end.hpp"

#include <iostream>

int main()
{
	// ==============================
	// 创建配置
	// ==============================

	etest::vision::CameraConfig camera_config;

	camera_config.source = "0";
	camera_config.width = 640;
	camera_config.height = 480;
	camera_config.fps = 60;

	// ==============================
	// 创建所有核心对象
	// ==============================

	etest::vision::Camera camera(camera_config);

	etest::vision::VisionProcessor vision;

	etest::Uart uart;

	// ==============================
	// 创建唯一的 Context
	// ==============================

	etest::AppContext ctx{camera, vision, uart, cv::Mat{}, {}, true};

	// ==============================
	// 初始化
	// ==============================

	if(!ctx.camera.open())
	{
		std::cerr << "[Main] Camera open failed\n";

		return 1;
	}

	// ==============================
	// 状态机 — 从 START 开始
	// ==============================

	etest::State current_state = etest::State::START;

	while(ctx.running)
	{
		switch(current_state)
		{
		case etest::State::START:

			current_state = etest::state::runStart(ctx);

			break;

		case etest::State::SEARCH:

			current_state = etest::state::runSearch(ctx);

			break;

		case etest::State::ERROR:

			current_state = etest::state::runError(ctx);

			break;

		case etest::State::END:

			etest::state::runEnd(ctx);

			break;
		}
	}

	ctx.camera.release();

	return 0;
}
