#pragma once

#include "core/config.hpp"

#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace etest::vision
{

enum class VisionMode
{
	Preview = 0,
	ColorTarget = 1,
	Line = 2,
	Circle = 3,
	Tag = 4,
	NeuralNetwork = 5,
	Ball = 6
};

enum class BallState
{
	FIND_TRACK,      // 寻找白色轨道
	CALIBRATE_ZERO,  // 零点标定
	TRACK_BALL,      // 正常追踪
	REACQUIRE_BALL   // 球丢失，重捕获
};

struct VisionResult
{
	bool valid = false;
	double x = 0.0;
	double y = 0.0;
	double angle = 0.0;
	double distance = 0.0;
	double confidence = 0.0;
	std::uint64_t frame_id = 0;
	std::int64_t timestamp_ms = 0;
	std::string target_type; // "RED_TARGET", "BALL" etc.
	std::string error_code;  // "" if valid, else reason

	// Ball 模式专用（仅 target_type=="BALL" 时有效）
	int offset_mm = 0;
	bool calibrated = false;
};

// 单次神经网络检测中识别出的一个物品。
struct DetectionInfo
{
	std::string class_name;
	float confidence = 0.0F;
	int x1 = 0;
	int y1 = 0;
	int x2 = 0;
	int y2 = 0;
	int x3 = 0;
	int y3 = 0;
	int x4 = 0;
	int y4 = 0;
};

// 白色轨道检测结果
struct TrackResult
{
	bool valid = false;
	cv::RotatedRect rect;
	cv::Rect bounding_roi;
	cv::Point2f axis_p1;
	cv::Point2f axis_p2;
	double confidence = 0.0;
};

class VisionProcessor
{
public:
	explicit VisionProcessor(VisionConfig config = {});

	VisionResult process(const cv::Mat& frame,
	                     VisionMode mode) noexcept;

	void drawDebugInfo(cv::Mat& frame,
	                   const VisionResult& result) noexcept;

	// 加载 ONNX 模型和类别文件。
	bool loadNnModel(const std::string& onnx_path,
	                 const std::string& class_names_path,
	                 double confidence_threshold,
	                 double nms_threshold) noexcept;

	// 对输入帧执行神经网络检测，返回绘制了检测框的帧。
	cv::Mat detectNn(const cv::Mat& frame) noexcept;

	// 返回最近一次 detectNn() 检测到的物品列表。
	const std::vector<DetectionInfo>& getLastDetections()
	    const noexcept;

	// 神经网络是否已加载。
	bool isNnLoaded() const noexcept;

private:
	VisionResult detectColorTarget(const cv::Mat& frame);
	VisionResult detectBall(const cv::Mat& frame);

	// ── 白色轨道检测 ──
	TrackResult detectWhiteTrack(const cv::Mat& frame) noexcept;
	bool isTrackSimilar(const TrackResult& a,
	                    const TrackResult& b) const noexcept;

	// Ball 调试绘制
	void drawBallDebugInfo(cv::Mat& frame,
	                       const VisionResult& result) noexcept;

	// 从轨道 bounding box 向内收缩生成球检测 ROI
	static cv::Rect makeInnerRoi(const cv::Rect& track_roi,
	                             const cv::Size& work_size) noexcept;

	VisionConfig config_;
	bool empty_frame_reported_ = false;
	std::uint64_t frame_id_counter_ = 0;

	// 神经网络相关。
	cv::dnn::Net nn_net_;
	bool nn_loaded_ = false;
	std::vector<std::string> nn_class_names_;
	double nn_confidence_threshold_ = 0.5;
	double nn_nms_threshold_ = 0.4;
	std::vector<std::string> nn_output_names_;

	// 最近一次 detectNn() 的检测结果。
	std::vector<DetectionInfo> last_detections_;

	// ── Ball 检测状态机 ──
	BallState ball_state_ = BallState::FIND_TRACK;

	bool ball_lost_ = false;
	int ball_lost_frame_count_ = 0;
	int track_lost_frame_count_ = 0;
	bool has_last_ball_center_ = false;
	cv::Point2f last_ball_center_{-1.0F, -1.0F};

	std::deque<double> zero_buffer_;
	bool zero_locked_ = false;
	double zero_position_px_ = 0.0;

	bool ball_filter_initialized_ = false;
	double filtered_offset_cm_ = 0.0;

	bool ball_config_error_reported_ = false;
	bool ball_resolution_reported_ = false;

	// ── 轨道锁定 ──
	TrackResult locked_track_;
	int track_stable_count_ = 0;
	bool track_locked_ = false;

	// ── 调试中间图像 ──
	cv::Mat debug_track_mask_;
	cv::Mat debug_ball_binary_;

	std::chrono::steady_clock::time_point last_ball_lost_log_time_{};
};

} // namespace etest::vision