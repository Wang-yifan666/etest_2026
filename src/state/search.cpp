#include "state/search.hpp"

#include "core/config.hpp"
#include "core/logger.hpp"

#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

#include <chrono>
#include <string>
#include <thread>

namespace etest::state
{

	State runSearch(AppContext& ctx, const SearchConfig& search_cfg)
	{
		// 节流日志 — 循环中使用 throttle，外层仍每次输出。
		ETEST_LOG_INFO("SEARCH", "entering search loop");

		if(search_cfg.enable_nn && !ctx.vision.isNnLoaded())
		{
			ETEST_LOG_INFO(
			    "SEARCH", "loading NN model: " + search_cfg.model_path);

			if(ctx.vision.loadNnModel(
			       search_cfg.model_path, search_cfg.class_names_path,
			       search_cfg.nn_confidence_threshold,
			       search_cfg.nn_nms_threshold))
			{
				ETEST_LOG_INFO("SEARCH",
				               "NN model loaded; detection enabled");
			}
			else
			{
				ETEST_LOG_WARN("SEARCH",
				               "NN model failed to load; "
				               "running without detection");
			}
		}

		const std::string preview_window = "Camera Preview";
		const std::string ctrl_window = "Control (ESC/q to exit)";

		bool preview_open = false;
		bool ctrl_open = false;
		bool show_preview = search_cfg.show_preview;

		// 节流日志的时间间隔。
		const int throttle_ms = 500;

		auto last_throttle_time = std::chrono::steady_clock::now()
		    - std::chrono::milliseconds(throttle_ms + 1);

		uint64_t frame_count = 0;

		const auto target_frame_interval =
		    std::chrono::milliseconds(33);

		auto last_frame_time = std::chrono::steady_clock::now();

		while(ctx.running)
		{
			++frame_count;

			const auto loop_start =
			    std::chrono::steady_clock::now();

			const auto since_last_frame =
			    std::chrono::duration_cast<std::chrono::milliseconds>(
			        loop_start - last_frame_time);

			if(since_last_frame < target_frame_interval)
			{
				std::this_thread::sleep_for(
				    target_frame_interval - since_last_frame);
			}

			// 1) 读取帧。
			if(!ctx.camera.read(ctx.frame))
			{
				ETEST_LOG_ERROR("SEARCH", "frame read failed");

				std::this_thread::sleep_for(
				    std::chrono::milliseconds(100));

				continue;
			}

			last_frame_time = std::chrono::steady_clock::now();

			// 2) 节流日志。
			const auto now = std::chrono::steady_clock::now();
			const auto elapsed =
			    std::chrono::duration_cast<std::chrono::milliseconds>(
			        now - last_throttle_time);

			if(elapsed.count() >= throttle_ms)
			{
				ETEST_LOG_INFO("SEARCH",
				               "searching... frame="
				                   + std::to_string(frame_count));

				last_throttle_time = now;
			}

			// 3) 构建显示帧。
			cv::Mat display_frame;

			if(search_cfg.enable_nn && ctx.vision.isNnLoaded())
			{
				cv::Mat detected = ctx.vision.detectNn(ctx.frame);

				cv::hconcat(ctx.frame, detected, display_frame);
			}
			else
			{
				display_frame = ctx.frame;
			}

			// 4) 预览窗口（可选关闭）。
			if(show_preview)
			{
				if(!preview_open)
				{
					cv::namedWindow(
					    preview_window,
					    cv::WINDOW_NORMAL | cv::WINDOW_KEEPRATIO);
					cv::resizeWindow(preview_window, 1280, 480);
					preview_open = true;
				}

				cv::imshow(preview_window, display_frame);
			}

			// 5) 控制面板 — 始终存在以确保 waitKey 能捕获按键。
			if(!ctrl_open)
			{
				cv::namedWindow(ctrl_window, cv::WINDOW_NORMAL);
				cv::resizeWindow(ctrl_window, 320, 50);
				ctrl_open = true;
			}

			{
				cv::Mat panel(50, 320, CV_8UC3,
				              cv::Scalar(45, 45, 45));
				cv::putText(panel, "ESC / q : exit program",
				            cv::Point(10, 25),
				            cv::FONT_HERSHEY_SIMPLEX, 0.55,
				            cv::Scalar(220, 220, 220), 1);
				cv::imshow(ctrl_window, panel);
			}

			// 6) 统一的按键处理。
			const int key = cv::waitKey(1) & 0xFF;

			if(key == 27 || key == 'q' || key == 'Q')
			{
				if(show_preview)
				{
					ETEST_LOG_INFO("SEARCH",
					               "preview window closed by user");
					cv::destroyWindow(preview_window);
					preview_open = false;
					show_preview = false;
				}
				else
				{
					ETEST_LOG_INFO("SEARCH",
					               "exit requested via keyboard");
					break;
				}
			}

		}

		cv::destroyAllWindows();

		ETEST_LOG_INFO("SEARCH", "exiting search loop");

		return State::END;
	}

} // namespace etest::state