#pragma once

#include "core/config.hpp"

#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace etest::vision
{

	class YoloDetector;

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
		FIND_TRACK,
		CALIBRATE_ZERO,
		TRACK_BALL,
		REACQUIRE_BALL
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
		std::string target_type;
		std::string error_code;
		int offset_mm = 0;
		int position_0p1mm = 0; // 0.1mm 单位，给新协议 BALL 行
		bool calibrated = false;
	};

	struct DetectionInfo
	{
		std::string class_name;
		float confidence = 0.0F;
		int x1 = 0, y1 = 0, x2 = 0, y2 = 0, x3 = 0, y3 = 0, x4 = 0,
		    y4 = 0;
	};

	// YOLO 检测结果
	struct YoloDetection
	{
		int class_id = -1;
		float confidence = 0.0F;
		cv::Rect box;

		cv::Point2f center() const noexcept
		{
			return {box.x + box.width * 0.5F,
			        box.y + box.height * 0.5F};
		}
	};

	// YOLO 推理各阶段耗时
	struct YoloTiming
	{
		double preprocess_ms = 0.0;
		double forward_ms = 0.0;
		double decode_ms = 0.0;
		double nms_ms = 0.0;
		double total_ms = 0.0;
	};

	// Letterbox 预处理信息
	struct LetterboxInfo
	{
		float scale = 1.0F;
		int pad_x = 0;
		int pad_y = 0;
	};

	struct TrackResult
	{
		bool valid = false;
		cv::RotatedRect rect;
		cv::Rect bounding_roi;
		cv::Point2f axis_p1;
		cv::Point2f axis_p2;
		double confidence = 0.0;
	};

	// reject_code: 0=PASSED, 1=BORDER, 2=CENTER_Y, 3=RADIUS, 4=TOO_BRIGHT,
	// 5=LOW_CONTRAST, 6=LOW_QUALITY, 7=MASK_TOO_SMALL
	struct BallCandidate
	{
		cv::Point2f center;
		float radius = 0.0F;
		double normalized_x = 0.0;
		double mean_inner_gray = 255.0;
		double mean_ring_gray = 255.0;
		double ring_contrast = 0.0;
		double radius_score = 0.0;
		double center_score = 0.0;
		double darkness_score = 0.0;
		double contrast_score = 0.0;
		double quality = 0.0;
		bool passed = false;
		int reject_code = 0;
		bool association_rejected = false;
	};

	class VisionProcessor
	{
	public:
		explicit VisionProcessor(VisionConfig config = {});
		~VisionProcessor();

		VisionResult process(const cv::Mat& frame,
		                     VisionMode mode) noexcept;

		void drawDebugInfo(cv::Mat& frame,
		                   const VisionResult& result) noexcept;

		bool loadNnModel(const std::string& onnx_path,
		                 const std::string& class_names_path,
		                 double confidence_threshold,
		                 double nms_threshold) noexcept;

		cv::Mat detectNn(const cv::Mat& frame) noexcept;

		const std::vector<DetectionInfo>& getLastDetections()
		    const noexcept;

		bool isNnLoaded() const noexcept;

		// ── YOLO 接口 ──
		void setVisionEpoch(std::uint64_t epoch_ns) noexcept;
		std::vector<YoloDetection> inferYolo(
		    const cv::Mat& frame,
		    YoloTiming* timing = nullptr) noexcept;
		VisionResult processYoloBall(
		    const cv::Mat& frame,
		    YoloTiming* timing = nullptr) noexcept;
		void resetYoloSession() noexcept;
		bool isYoloBallReady() const noexcept;
		void drawYoloDebugInfo(cv::Mat& frame,
		                       const VisionResult& result) noexcept;
		bool tryReloadYoloModel(
		    const std::string& model_path,
		    const std::string& class_names_path) noexcept;

		// ── YOLO 后端管理 ──
		bool loadYoloModel(const struct SearchConfig& config) noexcept;
		bool reloadYoloModel(
		    const struct SearchConfig& config) noexcept;
		const char* yoloBackendName() const noexcept;

	private:
		VisionResult detectColorTarget(const cv::Mat& frame);
		VisionResult detectBall(const cv::Mat& frame);

		TrackResult detectBrownPipe(const cv::Mat& frame) noexcept;
		bool isTrackSimilar(const TrackResult& a,
		                    const TrackResult& b) const noexcept;

		void drawBallDebugInfo(cv::Mat& frame,
		                       const VisionResult& result) noexcept;

		static void resizeLetterbox(const cv::Mat& source,
		                            cv::Mat& destination,
		                            cv::Scalar bg = cv::Scalar(0, 0,
		                                                       0));

	public:
		bool orderTrackCorners(
		    const cv::RotatedRect& rect,
		    std::array<cv::Point2f, 4>& ordered) noexcept;

		bool updateWarpMatrices() noexcept;

		std::vector<BallCandidate> detectBallCandidates(
		    const cv::Mat& warped_roi) noexcept;

	private:
		static cv::Rect makeInnerRoi(
		    const cv::Rect& track_roi,
		    const cv::Size& work_size) noexcept;

		VisionConfig config_;
		bool empty_frame_reported_ = false;
		std::uint64_t frame_id_counter_ = 0;

		cv::dnn::Net nn_net_;
		bool nn_loaded_ = false;
		std::vector<std::string> nn_class_names_;
		double nn_confidence_threshold_ = 0.5;
		double nn_nms_threshold_ = 0.4;
		std::vector<std::string> nn_output_names_;
		std::vector<DetectionInfo> last_detections_;

		BallState ball_state_ = BallState::FIND_TRACK;
		bool ball_lost_ = false;
		int ball_lost_frame_count_ = 0;
		int track_lost_frame_count_ = 0;

		std::deque<double> zero_buffer_;
		bool zero_locked_ = false;
		double zero_position_px_ = 0.0;

		TrackResult locked_track_;
		int track_stable_count_ = 0;
		bool track_locked_ = false;

		cv::Mat warp_matrix_;
		bool warp_locked_ = false;
		int warp_direction_ = 1;
		cv::Mat debug_warped_pipe_;

		std::array<cv::Point2f, 4> locked_pipe_points_{};
		bool locked_pipe_points_valid_ = false;
		cv::Mat inverse_warp_matrix_;

		cv::Mat debug_track_mask_;
		cv::Mat debug_ball_binary_;
		std::vector<BallCandidate> debug_ball_candidates_;

		// alpha-beta tracker
		bool tracker_initialized_ = false;
		double tracked_position_ratio_ = 0.0;
		double tracked_velocity_ratio_per_s_ = 0.0;
		double predicted_position_ratio_ = 0.0;
		int tracker_lost_frames_ = 0;
		int reacquire_confirm_count_ = 0;
		double reacquire_candidate_ratio_ = 0.0;
		int acquire_confirm_count_ = 0;
		double acquire_candidate_ratio_ = 0.0;
		std::chrono::steady_clock::time_point tracker_last_time_;

		std::chrono::steady_clock::time_point
		    last_ball_lost_log_time_{};
		std::chrono::steady_clock::time_point
		    last_corner_order_error_time_{};
		std::chrono::steady_clock::time_point
		    last_warp_update_error_time_{};
		std::chrono::steady_clock::time_point
		    last_pipe_lost_warn_time_{};
		std::chrono::steady_clock::time_point
		    last_pipe_recovered_info_time_{};

		std::string last_ball_error_code_;
		std::chrono::steady_clock::time_point last_ball_error_time_{};
		int ball_lost_frames_on_recover_ = 0;

		// ── YOLO 会话状态 ──
		std::uint64_t vision_epoch_ns_ = 0;
		bool yolo_shape_logged_ = false;

		std::deque<cv::Point2f> yolo_zero_samples_;
		bool yolo_zero_locked_ = false;
		cv::Point2f yolo_zero_origin_{0.0F, 0.0F};
		int yolo_calib_frame_count_ = 0;

		int yolo_lost_frames_ = 0;
		int yolo_reacquire_confirm_ = 0;
		cv::Point2f yolo_last_center_{-1.0F, -1.0F};
		bool yolo_tracking_initialized_ = false;

		double yolo_filtered_x_ = 0.0;

		int yolo_consecutive_errors_ = 0;
		bool yolo_model_unhealthy_ = false;
		std::chrono::steady_clock::time_point yolo_last_reload_attempt_;
		std::chrono::steady_clock::time_point yolo_lost_log_time_;
		std::string yolo_last_error_code_;
		std::chrono::steady_clock::time_point yolo_last_error_time_;

		std::vector<YoloDetection> yolo_last_detections_;
		double yolo_last_valid_confidence_ = 0.0;

		// ── YOLO 后端检测器 ──
		std::unique_ptr<YoloDetector> yolo_detector_;
	};

} // namespace etest::vision