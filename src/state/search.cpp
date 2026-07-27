#include "state/search.hpp"

#include "core/config.hpp"
#include "core/logger.hpp"
#include "uart/protocol.hpp"

#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

#include <chrono>
#include <sstream>
#include <string>
#include <thread>

namespace etest::state
{

	State runSearch(AppContext& ctx, const SearchConfig& search_cfg,
	                bool allow_keyboard_exit)
	{
		ETEST_LOG_INFO("SEARCH", "entering search loop");

		const bool show_preview = search_cfg.show_preview;

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
		bool preview_open = false;

		const int throttle_ms = 500;
		auto last_throttle_time = std::chrono::steady_clock::now()
		    - std::chrono::milliseconds(throttle_ms + 1);

		uint64_t frame_count = 0;

		const auto target_frame_interval =
		    std::chrono::milliseconds(33);
		auto last_frame_time = std::chrono::steady_clock::now();

		while(ctx.running)
		{
			if(ctx.shutdown_flag != nullptr
			   && ctx.shutdown_flag->load())
			{
				ETEST_LOG_INFO("SEARCH", "shutdown signal detected");
				break;
			}

			++frame_count;

			const auto loop_start = std::chrono::steady_clock::now();
			const auto since_last_frame =
			    std::chrono::duration_cast<std::chrono::milliseconds>(
			        loop_start - last_frame_time);

			if(since_last_frame < target_frame_interval)
			{
				std::this_thread::sleep_for(target_frame_interval
				                            - since_last_frame);
			}

			// 1) 读取帧
			if(!ctx.camera.read(ctx.frame))
			{
				if(ctx.camera.getState()
				   == vision::CameraState::FILE_EOF)
				{
					ETEST_LOG_INFO("SEARCH",
					               "file source ended; exiting search");
					break;
				}

				ETEST_LOG_ERROR("SEARCH", "frame read failed");

				ctx.last_fault = {
				    FaultSource::CAMERA, RecoveryAction::REOPEN_CAMERA,
				    "SEARCH_FRAME_READ", "frame read failed in SEARCH"};
				return State::ERROR;
			}

			last_frame_time = std::chrono::steady_clock::now();

			// 2) 执行视觉算法（无论是否预览）
			ctx.vision_result = ctx.vision.process(
			    ctx.frame, vision::VisionMode::ColorTarget);

			// 3) 发送目标结果（无论是否预览）
			if(ctx.uart.isOpen())
			{
				if(ctx.vision_result.valid)
				{
					std::ostringstream payload;
					payload << ctx.vision_result.x << ";"
					        << ctx.vision_result.y << ";"
					        << ctx.vision_result.angle << ";"
					        << ctx.vision_result.confidence;

					const std::string frame =
					    uart::protocol::encodeMessage(
					        1, "TARGET", ++ctx.uart_seq, payload.str());

					ctx.uart.sendLine(frame);
				}
				else
				{
					const std::string frame =
					    uart::protocol::encodeMessage(
					        1, "LOST", ++ctx.uart_seq, "0");

					ctx.uart.sendLine(frame);
				}
			}

			// 4) 节流日志
			const auto now = std::chrono::steady_clock::now();
			const auto elapsed =
			    std::chrono::duration_cast<std::chrono::milliseconds>(
			        now - last_throttle_time);

			if(elapsed.count() >= throttle_ms)
			{
				std::string msg =
				    "searching... frame=" + std::to_string(frame_count);

				if(ctx.vision_result.valid)
				{
					msg += " | target: ("
					    + std::to_string(ctx.vision_result.x) + ","
					    + std::to_string(ctx.vision_result.y)
					    + ") angle="
					    + std::to_string(ctx.vision_result.angle);
				}
				else
				{
					msg += " | target: LOST";
				}

				ETEST_LOG_INFO("SEARCH", msg);

				last_throttle_time = now;
			}

			// 5) 仅预览模式绘制和键盘
			if(show_preview)
			{
				cv::Mat display = ctx.frame.clone();
				ctx.vision.drawDebugInfo(display, ctx.vision_result);

				if(!preview_open)
				{
					cv::namedWindow(
					    preview_window,
					    cv::WINDOW_NORMAL | cv::WINDOW_KEEPRATIO);
					cv::resizeWindow(preview_window, 1280, 480);
					preview_open = true;
				}

				cv::imshow(preview_window, display);

				const int key = cv::waitKey(1) & 0xFF;

				if(allow_keyboard_exit
				   && (key == 27 || key == 'q' || key == 'Q'))
				{
					ETEST_LOG_INFO("SEARCH",
					               "exit requested via keyboard");
					break;
				}
			}
			else
			{
				std::this_thread::sleep_for(
				    std::chrono::milliseconds(1));
			}
		}

		if(show_preview && preview_open)
		{
			cv::destroyAllWindows();
		}

		ETEST_LOG_INFO("SEARCH", "exiting search loop");

		return State::END;
	}

} // namespace etest::state