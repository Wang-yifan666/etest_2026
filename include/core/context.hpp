#pragma once

#include <atomic>
#include <opencv2/opencv.hpp>

#include "core/config.hpp"
#include "state/state.hpp"
#include "uart/uart.hpp"
#include "vision/camera.hpp"
#include "vision/video_recorder.hpp"
#include "vision/vision.hpp"

namespace etest
{

	enum class ExitReason
	{
		NORMAL,
		SAFE_STOP,
		RESTART_REQUIRED
	};

	// 任务阶段（SEARCH 内部）
	enum class TaskPhase
	{
		CALIBRATING, // 标定原点中，发送 BALL CALIB
		RUNNING,     // 标定完成，等待发送 CONTESTSTART
		CONTEST,     // 比赛进行中，收到 CONTESTSTART ACK 后
		STOPPING     // 收到 DONE，等待发送 CONTESTSTOP 后回到 RUNNING
	};

	// 任务会话
	struct TaskSession
	{
		std::uint32_t session_id = 0;         // 视觉会话 ID
		std::uint64_t vision_epoch_ns = 0;    // 会话零点（单调时钟）
		std::uint32_t seq = 0;                // BALL 序号
		bool vsession_confirmed = false;      // VSESSION 握手完成
		std::string active_mode;              // 当前比赛模式
		int command = 0;                      // 任务命令号 1~5
		int problem_number = 0;               // 题目编号 2~6
		bool mission_received = false;        // 是否收到 M000X 选题
		bool contest_start_sent = false;      // CONTESTSTART 已发送
		bool contest_start_acked = false;     // CONTESTSTART 已确认
		std::chrono::steady_clock::time_point phase_since{};
	};

	struct AppContext
	{
		vision::Camera& camera;
		vision::VisionProcessor& vision;
		vision::VideoRecorder& recorder;
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

		// 任务状态
		TaskSession task;
		TaskPhase task_phase = TaskPhase::CALIBRATING;
	};

	// 纯函数：根据连续异常计数返回恢复动作
	RecoveryAction faultActionForConsecutiveCount(int count);

	// 纯函数：退出原因映射到退出码
	int exitCodeForReason(ExitReason reason);

} // namespace etest