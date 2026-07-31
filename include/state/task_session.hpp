#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#include <opencv2/core.hpp>

namespace etest
{

// 题目模式
enum class TaskMode
{
	NONE,
	Q2, // M0001
	Q3, // M0002
	Q4, // M0003
	Q5, // M0004
	Q6  // M0005
};

// 比赛任务阶段（与应用状态 START/SEARCH/ERROR/END 正交）
enum class ContestTaskPhase
{
	IDLE,        // 等待 M000x
	PREPARING,   // M0001 瞬时确认
	CALIBRATING, // M0002～M0005 标定
	RUNNING,     // 正式运行
	FINISHED,    // 收到匹配的 DONE
	FAULT        // 不可恢复错误
};

// 视觉跟踪模式（与 RUNNING 正交）
enum class TrackingMode
{
	NONE,           // M0001 不推理
	FULL,           // M0002/M0005 完整模型
	CENTER,         // M0003/M0004 中心模型
	FULL_REACQUIRE  // 中心丢球后切完整模型找回
};

// 物理输出单次测量
struct BallMeasurement
{
	bool valid = false;

	// ROI 局部像素坐标
	cv::Point2f local_center{0.0F, 0.0F};

	// 原图全局像素坐标
	cv::Point2f global_center{0.0F, 0.0F};

	// 物理坐标（0.1 mm 单位，相对固定中心 O）
	int position_0p1mm = 0;

	// 置信度 0.0～1.0
	float confidence = 0.0F;

	// 状态
	std::string status = "LOST";

	// 推理耗时 ms
	double inference_ms = 0.0;
};

// 任务会话
struct TaskSession
{
	TaskMode mode = TaskMode::NONE;
	ContestTaskPhase phase = ContestTaskPhase::IDLE;
	TrackingMode tracking_mode = TrackingMode::NONE;

	std::string command_tag;     // "M0001"～"M0005"
	std::uint64_t session_id = 0;

	bool vision_enabled = false;
	bool start_sent = false;
	bool measurement_valid = false;

	// M0005 专用
	double target_position_mm = 0.0;

	// 当前测量
	double current_position_mm = 0.0;
	double last_confidence = 0.0;
	cv::Point2f last_global_center{-1.0F, -1.0F};

	// 序号
	std::uint32_t seq = 0;

	// VSESSION
	bool vsession_confirmed = false;

	// 标定
	int calibration_valid_frames = 0;
	std::chrono::steady_clock::time_point
	    calibration_start_time{};

	// 跟踪丢失计数
	int lost_frames = 0;

	// 中心模型稳定帧数（完整模型找回后计数）
	int center_stable_frames = 0;

	// 滤波
	double filtered_x = 0.0;
	double filtered_y = 0.0;

	// 探测到球中心（用于距离计算）
	double current_ball_global_x = 0.0;
	double current_ball_global_y = 0.0;

	// ── 纯函数 ──

	// 重置会话（session_id 递增）
	void reset();

	// 进入指定阶段
	void enterPhase(ContestTaskPhase new_phase);

	// M000x → TaskMode 映射
	static TaskMode modeFromTag(const std::string& tag);

	// TaskMode → 模式名 "H2"～"H6"
	static const char* modeName(TaskMode m);
};

} // namespace etest