#pragma once

#include <atomic>
#include <opencv2/opencv.hpp>

#include "core/config.hpp"
#include "state/state.hpp"
#include "uart/uart.hpp"
#include "vision/camera.hpp"
#include "vision/vision.hpp"

namespace etest
{

	enum class ExitReason
	{
		NORMAL,
		SAFE_STOP,
		RESTART_REQUIRED
	};

	struct AppContext
	{
		vision::Camera& camera;
		vision::VisionProcessor& vision;
		Uart& uart;

		cv::Mat frame;
		vision::VisionResult vision_result;

		bool running = true;

		// 故障追踪
		FaultInfo last_fault;
		int consecutive_exceptions = 0;
		static constexpr int kMaxConsecutiveExceptions = 10;
		int error_state_entry_count = 0;

		// 退出原因
		ExitReason exit_reason = ExitReason::NORMAL;

		// 信号处理（由 main 设置，主循环检测）
		std::atomic_bool* shutdown_flag = nullptr;

		// UART 协议
		bool lower_machine_online = false;
		std::uint32_t uart_seq = 0;
	};

	// 纯函数：根据连续异常计数返回恢复动作
	RecoveryAction faultActionForConsecutiveCount(int count);

	// 纯函数：退出原因映射到退出码
	int exitCodeForReason(ExitReason reason);

} // namespace etest