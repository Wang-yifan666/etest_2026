#include "vision/vision.hpp"

#include "core/logger.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <opencv2/imgproc.hpp>

namespace etest::vision
{

VisionProcessor::VisionProcessor(VisionConfig config):
config_(std::move(config))
{
}

VisionResult VisionProcessor::process(const cv::Mat& frame,
                                      VisionMode mode) noexcept
{
	++frame_id_counter_;

	const auto now = std::chrono::steady_clock::now();
	const auto timestamp_ms =
	    std::chrono::duration_cast<std::chrono::milliseconds>(
	        now.time_since_epoch())
	        .count();

	if(frame.empty())
	{
		if(!empty_frame_reported_)
		{
			ETEST_LOG_ERROR("VISION", "received an empty frame");
			empty_frame_reported_ = true;
		}

		VisionResult result;
		result.frame_id = frame_id_counter_;
		result.timestamp_ms = static_cast<std::int64_t>(timestamp_ms);
		result.target_type = (mode == VisionMode::Ball) ? "BALL" : "";
		result.error_code = "EMPTY_FRAME";
		return result;
	}

	if(empty_frame_reported_)
	{
		ETEST_LOG_INFO("VISION", "valid frame input recovered");
		empty_frame_reported_ = false;
	}

	VisionResult result;
	result.frame_id = frame_id_counter_;
	result.timestamp_ms = static_cast<std::int64_t>(timestamp_ms);

	if(mode == VisionMode::Ball)
	{
		result.target_type = "BALL";
	}

	try
	{
		switch(mode)
		{
		case VisionMode::ColorTarget:
			result = detectColorTarget(frame);
			result.frame_id = frame_id_counter_;
			result.timestamp_ms =
			    static_cast<std::int64_t>(timestamp_ms);
			return result;

		case VisionMode::Ball:
			result = detectBall(frame);
			result.frame_id = frame_id_counter_;
			result.timestamp_ms =
			    static_cast<std::int64_t>(timestamp_ms);
			return result;

		case VisionMode::Preview:
		case VisionMode::Line:
		case VisionMode::Circle:
		case VisionMode::Tag:
		case VisionMode::NeuralNetwork:
		default:
			result.error_code = "UNSUPPORTED_MODE";
			return result;
		}
	}
	catch(const cv::Exception& error)
	{
		ETEST_LOG_ERROR("VISION",
		                std::string("OpenCV processing exception: ")
		                    + error.what());
		result.valid = false;
		result.error_code = "CV_EXCEPTION";
		return result;
	}
	catch(const std::exception& error)
	{
		ETEST_LOG_ERROR(
		    "VISION",
		    std::string("processing exception: ") + error.what());
		result.valid = false;
		result.error_code = "STD_EXCEPTION";
		return result;
	}
	catch(...)
	{
		ETEST_LOG_ERROR("VISION", "unknown processing exception");
		result.valid = false;
		result.error_code = "UNKNOWN_EXCEPTION";
		return result;
	}
}

VisionResult VisionProcessor::detectColorTarget(
    const cv::Mat& frame)
{
	VisionResult result;
	result.target_type = "RED_TARGET";

	cv::Mat hsv;
	cv::cvtColor(frame, hsv, cv::COLOR_BGR2HSV);

	cv::Mat mask1;
	cv::Mat mask2;
	cv::inRange(
	    hsv,
	    cv::Scalar(config_.red_h1_min, config_.saturation_min,
	               config_.value_min),
	    cv::Scalar(config_.red_h1_max, 255, 255), mask1);
	cv::inRange(
	    hsv,
	    cv::Scalar(config_.red_h2_min, config_.saturation_min,
	               config_.value_min),
	    cv::Scalar(config_.red_h2_max, 255, 255), mask2);

	cv::Mat mask = mask1 | mask2;

	const cv::Mat kernel = cv::getStructuringElement(
	    cv::MORPH_ELLIPSE,
	    cv::Size(config_.morphology_kernel,
	             config_.morphology_kernel));
	cv::morphologyEx(mask, mask, cv::MORPH_OPEN, kernel);
	cv::morphologyEx(mask, mask, cv::MORPH_CLOSE, kernel);

	std::vector<std::vector<cv::Point>> contours;
	cv::findContours(mask, contours, cv::RETR_EXTERNAL,
	                 cv::CHAIN_APPROX_SIMPLE);

	if(contours.empty())
	{
		result.error_code = "NO_CONTOUR";
		return result;
	}

	const auto largest = std::max_element(
	    contours.begin(), contours.end(),
	    [](const auto& left, const auto& right) {
		    return cv::contourArea(left) < cv::contourArea(right);
	    });

	const double area = cv::contourArea(*largest);
	if(area < config_.min_area)
	{
		result.error_code = "AREA_TOO_SMALL";
		return result;
	}

	const cv::Moments moments = cv::moments(*largest);
	if(moments.m00 == 0.0)
	{
		ETEST_LOG_WARN("VISION", "largest contour has zero moment");
		result.error_code = "ZERO_MOMENT";
		return result;
	}

	result.valid = true;
	result.x = moments.m10 / moments.m00;
	result.y = moments.m01 / moments.m00;
	result.confidence = 1.0;

	const cv::RotatedRect rectangle = cv::minAreaRect(*largest);
	result.angle = rectangle.angle;
	result.distance = std::sqrt(area);

	return result;
}

// ────────────────────────────────────────────────────────────
// 白色轨道检测
// ────────────────────────────────────────────────────────────

TrackResult VisionProcessor::detectWhiteTrack(
    const cv::Mat& frame) noexcept
{
	TrackResult result;

	try
	{
		if(frame.empty())
		{
			ETEST_LOG_ERROR("TRACK",
			                "detectWhiteTrack received empty frame");
			return result;
		}

		const auto& ball = config_.ball;

		cv::Mat hsv;
		cv::cvtColor(frame, hsv, cv::COLOR_BGR2HSV);

		cv::Mat mask;
		cv::inRange(
		    hsv,
		    cv::Scalar(0, 0, ball.white_v_min),
		    cv::Scalar(180, ball.white_s_max, 255),
		    mask);

		const cv::Mat close_kernel =
		    cv::getStructuringElement(
		        cv::MORPH_RECT, cv::Size(21, 7));
		const cv::Mat open_kernel =
		    cv::getStructuringElement(
		        cv::MORPH_RECT, cv::Size(5, 5));

		cv::morphologyEx(mask, mask, cv::MORPH_CLOSE,
		                 close_kernel);
		cv::morphologyEx(mask, mask, cv::MORPH_OPEN,
		                 open_kernel);

		debug_track_mask_ = mask.clone();

		std::vector<std::vector<cv::Point>> contours;
		cv::findContours(mask, contours, cv::RETR_EXTERNAL,
		                 cv::CHAIN_APPROX_SIMPLE);

		const double image_area =
		    static_cast<double>(frame.cols) * frame.rows;

		double best_score = -1.0;
		cv::RotatedRect best_rect;

		for(const auto& contour: contours)
		{
			const double area = cv::contourArea(contour);
			if(area
			   < image_area * ball.track_min_area_ratio)
			{
				continue;
			}

			const cv::RotatedRect rect =
			    cv::minAreaRect(contour);

			const double width = rect.size.width;
			const double height = rect.size.height;
			const double long_side =
			    std::max(width, height);
			const double short_side =
			    std::min(width, height);

			if(short_side < 5.0) continue;

			const double aspect = long_side / short_side;
			if(aspect < ball.track_min_aspect_ratio)
				continue;

			if(rect.center.y < frame.rows * 0.20
			   || rect.center.y > frame.rows * 0.90)
				continue;

			const double score =
			    area * std::min(aspect, 15.0);

			if(score > best_score)
			{
				best_score = score;
				best_rect = rect;
			}
		}

		if(best_score < 0.0) return result;

		cv::Point2f points[4];
		best_rect.points(points);

		const double edge01 = cv::norm(points[1] - points[0]);
		const double edge12 = cv::norm(points[2] - points[1]);

		cv::Point2f axis_p1, axis_p2;
		if(edge01 >= edge12)
		{
			axis_p1 = (points[0] + points[3]) * 0.5F;
			axis_p2 = (points[1] + points[2]) * 0.5F;
		}
		else
		{
			axis_p1 = (points[0] + points[1]) * 0.5F;
			axis_p2 = (points[2] + points[3]) * 0.5F;
		}

		cv::Rect roi = best_rect.boundingRect();
		roi &= cv::Rect(0, 0, frame.cols, frame.rows);

		if(roi.empty())
		{
			ETEST_LOG_ERROR("TRACK",
			                "detected track produced empty ROI");
			return result;
		}

		result.valid = true;
		result.rect = best_rect;
		result.bounding_roi = roi;
		result.axis_p1 = axis_p1;
		result.axis_p2 = axis_p2;
		result.confidence =
		    std::clamp(best_score / image_area, 0.0, 1.0);

		return result;
	}
	catch(const cv::Exception& error)
	{
		ETEST_LOG_ERROR(
		    "TRACK",
		    std::string("OpenCV exception: ") + error.what());
	}
	catch(const std::exception& error)
	{
		ETEST_LOG_ERROR(
		    "TRACK",
		    std::string("exception: ") + error.what());
	}
	catch(...)
	{
		ETEST_LOG_ERROR("TRACK", "unknown exception");
	}

	return result;
}

bool VisionProcessor::isTrackSimilar(
    const TrackResult& a, const TrackResult& b) const noexcept
{
	if(!a.valid || !b.valid) return false;

	const double center_distance =
	    cv::norm(a.rect.center - b.rect.center);

	const double length_a =
	    std::max(a.rect.size.width, a.rect.size.height);
	const double length_b =
	    std::max(b.rect.size.width, b.rect.size.height);

	return center_distance < 10.0
	    && std::abs(length_a - length_b) < 20.0;
}

cv::Rect VisionProcessor::makeInnerRoi(
    const cv::Rect& track_roi,
    const cv::Size& work_size) noexcept
{
	const int margin_x =
	    std::max(1, static_cast<int>(track_roi.width * 0.03));
	const int margin_y =
	    std::max(1, static_cast<int>(track_roi.height * 0.15));

	cv::Rect inner(
	    track_roi.x + margin_x,
	    track_roi.y + margin_y,
	    track_roi.width - margin_x * 2,
	    track_roi.height - margin_y * 2);

	inner &= cv::Rect(0, 0, work_size.width, work_size.height);

	if(inner.width < 10) inner.width = 10;
	if(inner.height < 10) inner.height = 10;

	return inner;
}

// ────────────────────────────────────────────────────────────
// Ball 检测（统一入口，内含状态机）
// ────────────────────────────────────────────────────────────

VisionResult VisionProcessor::detectBall(const cv::Mat& frame)
{
	VisionResult result;
	result.target_type = "BALL";

	// 0) 统一工作分辨率 640×480
	constexpr int kWorkWidth = 640;
	constexpr int kWorkHeight = 480;

	cv::Mat work_frame;

	if(frame.cols != kWorkWidth || frame.rows != kWorkHeight)
	{
		if(!ball_resolution_reported_)
		{
			ETEST_LOG_INFO(
			    "VISION",
			    "resizing input from "
			        + std::to_string(frame.cols) + "x"
			        + std::to_string(frame.rows)
			        + " to work resolution "
			        + std::to_string(kWorkWidth) + "x"
			        + std::to_string(kWorkHeight));
			ball_resolution_reported_ = true;
		}

		cv::resize(frame, work_frame,
		           cv::Size(kWorkWidth, kWorkHeight),
		           0.0, 0.0, cv::INTER_LINEAR);
	}
	else
	{
		work_frame = frame;
	}

	const auto& ball = config_.ball;

	// ── 1) 轨道检测 → 维护锁定状态 ──
	if(!track_locked_)
	{
		const TrackResult track =
		    detectWhiteTrack(work_frame);

		if(track.valid)
		{
			if(isTrackSimilar(track, locked_track_))
			{
				++track_stable_count_;
				if(track_stable_count_
				   >= ball.track_stable_frames)
				{
					track_locked_ = true;
					locked_track_ = track;
					track_lost_frame_count_ = 0;
					ETEST_LOG_INFO(
					    "VISION",
					    "track geometry locked");
				}
			}
			else
			{
				track_stable_count_ = 1;
				locked_track_ = track;
			}
		}
		else
		{
			track_stable_count_ = 0;
		}
	}
	else
	{
		const TrackResult track =
		    detectWhiteTrack(work_frame);

		if(!track.valid
		   || !isTrackSimilar(track, locked_track_))
		{
			++track_lost_frame_count_;
			if(track_lost_frame_count_
			   >= ball.track_lost_timeout_frames)
			{
				track_locked_ = false;
				track_stable_count_ = 0;
				track_lost_frame_count_ = 0;
				ball_state_ = BallState::FIND_TRACK;
				zero_locked_ = false;
				zero_buffer_.clear();
				ball_filter_initialized_ = false;
				ETEST_LOG_WARN(
				    "VISION",
				    "track lost; resetting to FIND_TRACK");
			}
		}
		else
		{
			track_lost_frame_count_ = 0;
			locked_track_ = track;
		}
	}

	// ── 2) 轨道未锁定 → 不能找球 ──
	if(!track_locked_)
	{
		result.x = 0.0;
		result.y = 0.0;
		result.error_code = "TRACK_NOT_LOCKED";
		return result;
	}

	// ── 3) 动态 ROI 和轴线 ──
	const cv::Rect ball_roi =
	    makeInnerRoi(locked_track_.bounding_roi,
	                 work_frame.size());

	const cv::Point2f axis_p1 = locked_track_.axis_p1;
	const cv::Point2f axis_p2 = locked_track_.axis_p2;
	const cv::Point2f axis_vec = axis_p2 - axis_p1;
	const double axis_len = cv::norm(axis_vec);

	if(axis_len < 1.0)
	{
		result.error_code = "INVALID_AXIS";
		return result;
	}

	const cv::Point2f axis_unit =
	    axis_vec / static_cast<float>(axis_len);

	// ── 4) ROI 裁剪 → 灰度 → 局部背景差分 ──
	const cv::Mat roi_image = work_frame(ball_roi);

	cv::Mat gray;
	if(roi_image.channels() == 3)
		cv::cvtColor(roi_image, gray, cv::COLOR_BGR2GRAY);
	else
		gray = roi_image.clone();

	cv::GaussianBlur(gray, gray, cv::Size(5, 5), 0.0);

	cv::Mat background;
	cv::GaussianBlur(gray, background,
	                 cv::Size(ball.bg_kernel, ball.bg_kernel),
	                 0.0);

	cv::Mat diff;
	cv::absdiff(gray, background, diff);

	cv::Mat binary;
	cv::threshold(diff, binary, ball.threshold, 255,
	              cv::THRESH_BINARY);

	const cv::Mat morph_kernel = cv::getStructuringElement(
	    cv::MORPH_ELLIPSE,
	    cv::Size(ball.morph_kernel, ball.morph_kernel));
	cv::morphologyEx(binary, binary, cv::MORPH_CLOSE,
	                 morph_kernel);
	cv::morphologyEx(binary, binary, cv::MORPH_OPEN,
	                 morph_kernel);

	debug_ball_binary_ = binary.clone();

	// ── 5) 轮廓筛选 ──
	std::vector<std::vector<cv::Point>> contours;
	cv::findContours(binary, contours, cv::RETR_EXTERNAL,
	                 cv::CHAIN_APPROX_SIMPLE);

	double best_score = -1e9;
	double best_circularity = 0.0;
	cv::Point2f best_center;

	const bool in_reacquire =
	    (ball_state_ == BallState::REACQUIRE_BALL);

	for(const auto& contour: contours)
	{
		const double area = cv::contourArea(contour);
		if(area < ball.min_area || area > ball.max_area)
			continue;

		const double perimeter =
		    cv::arcLength(contour, true);
		if(perimeter < 1.0) continue;

		const double circularity =
		    4.0 * CV_PI * area
		    / (perimeter * perimeter);
		if(circularity < ball.min_circularity)
			continue;

		const cv::Rect box = cv::boundingRect(contour);
		const double aspect =
		    static_cast<double>(box.width)
		    / std::max(1, box.height);
		if(aspect < 0.70 || aspect > 1.40)
			continue;

		const cv::Moments moments = cv::moments(contour);
		if(std::abs(moments.m00) < 1e-6) continue;

		const cv::Point2f center(
		    static_cast<float>(
		        moments.m10 / moments.m00
		        + ball_roi.x),
		    static_cast<float>(
		        moments.m01 / moments.m00
		        + ball_roi.y));

		const cv::Point2f from_axis = center - axis_p1;
		const double projected =
		    from_axis.x * axis_unit.x
		    + from_axis.y * axis_unit.y;

		if(projected < 0.0 || projected > axis_len)
			continue;

		const cv::Point2f nearest =
		    axis_p1
		    + axis_unit * static_cast<float>(projected);
		const double axis_distance =
		    cv::norm(center - nearest);

		if(axis_distance > ball.max_axis_distance_px)
			continue;

		double jump_distance = 0.0;
		if(!in_reacquire && has_last_ball_center_)
		{
			jump_distance = cv::norm(
			    center - last_ball_center_);
			if(jump_distance > ball.max_jump_px)
				continue;
		}

		const double score =
		    circularity * 100.0
		    - axis_distance * 1.5
		    - jump_distance * 0.2;

		if(score > best_score)
		{
			best_score = score;
			best_center = center;
			best_circularity = circularity;
		}
	}

	// ── 6) 无候选 → 状态机处理 ──
	if(best_score < -1e7)
	{
		++ball_lost_frame_count_;

		if(!ball_lost_)
		{
			ball_lost_ = true;
			ETEST_LOG_WARN("VISION", "Ball lost");
			last_ball_lost_log_time_ =
			    std::chrono::steady_clock::now();
		}
		else
		{
			const auto now =
			    std::chrono::steady_clock::now();
			const auto elapsed =
			    std::chrono::duration_cast<
			        std::chrono::milliseconds>(
			        now - last_ball_lost_log_time_);
			if(elapsed.count() >= 1000)
			{
				ETEST_LOG_WARN(
				    "VISION",
				    "Ball remains lost, frames="
				        + std::to_string(
				            ball_lost_frame_count_));
				last_ball_lost_log_time_ = now;
			}
		}

		if(ball_lost_frame_count_
		   >= ball.reacquire_after_lost_frames)
		{
			if(ball_state_ == BallState::TRACK_BALL
			   || ball_state_
			       == BallState::CALIBRATE_ZERO)
			{
				ball_state_ =
				    BallState::REACQUIRE_BALL;
				has_last_ball_center_ = false;
				ball_filter_initialized_ = false;
			}
		}

		if(!zero_locked_) zero_buffer_.clear();

		result.x = 0.0;
		result.y = 0.0;
		result.error_code = "BALL_LOST";
		return result;
	}

	// ── 7) 检测成功 → 重置丢球计数 ──
	ball_lost_frame_count_ = 0;

	if(ball_lost_)
	{
		ball_lost_ = false;
		ETEST_LOG_INFO("VISION", "Ball recovered");
	}

	has_last_ball_center_ = true;
	last_ball_center_ = best_center;

	result.x = best_center.x;
	result.y = best_center.y;

	const cv::Point2f from_start = best_center - axis_p1;
	const double axis_position_px =
	    from_start.x * axis_unit.x
	    + from_start.y * axis_unit.y;

	// ── 8) 重捕获恢复 → 回到 TRACK_BALL ──
	if(ball_state_ == BallState::REACQUIRE_BALL)
	{
		ball_state_ = BallState::TRACK_BALL;
		// ball_lost_frame_count_ 已在上面重置为 0
		ETEST_LOG_INFO(
		    "VISION",
		    "Ball reacquired; resuming TRACK");
	}

	// ── 9) 零点校准 ──
	if(!zero_locked_)
	{
		if(ball_state_ == BallState::FIND_TRACK)
		{
			ball_state_ = BallState::CALIBRATE_ZERO;
		}

		if(ball.zero_mode == "fixed")
		{
			zero_position_px_ = ball.zero_position_px;
			zero_locked_ = true;
			ball_state_ = BallState::TRACK_BALL;
			ETEST_LOG_INFO(
			    "VISION",
			    "Ball zero locked (fixed): "
			        + std::to_string(zero_position_px_)
			        + " px");
		}
		else
		{
			if(!zero_buffer_.empty())
			{
				const double last =
				    zero_buffer_.back();
				if(std::abs(axis_position_px - last)
				   > ball.max_jump_px)
				{
					zero_buffer_.clear();
				}
			}

			zero_buffer_.push_back(axis_position_px);

			while(static_cast<int>(
			          zero_buffer_.size())
			      > ball.zero_samples)
			{
				zero_buffer_.pop_front();
			}

			if(static_cast<int>(
			       zero_buffer_.size())
			   >= ball.zero_samples)
			{
				std::vector<double> values(
				    zero_buffer_.begin(),
				    zero_buffer_.end());
				std::sort(values.begin(),
				          values.end());

				const double median =
				    values[values.size() / 2];
				const double range =
				    values.back()
				    - values.front();

				if(range <= ball.zero_range_px)
				{
					zero_position_px_ = median;
					zero_locked_ = true;
					ball_state_ =
					    BallState::TRACK_BALL;
					ETEST_LOG_INFO(
					    "VISION",
					    "Ball zero calibrated: median="
					        + std::to_string(
					            median)
					        + " px, range="
					        + std::to_string(
					            range));
				}
			}
		}
	}

	if(!zero_locked_)
	{
		result.error_code = "ZERO_CALIBRATING";
		return result;
	}

	// ── 10) 厘米换算 + 低通滤波 ──
	const double pixels_per_cm =
	    axis_len / ball.axis_length_cm;
	const double raw_offset_cm =
	    (axis_position_px - zero_position_px_)
	    / pixels_per_cm;

	if(!ball_filter_initialized_)
	{
		filtered_offset_cm_ = raw_offset_cm;
		ball_filter_initialized_ = true;
	}
	else
	{
		const double alpha =
		    std::clamp(ball.filter_alpha, 0.01, 1.0);
		filtered_offset_cm_ =
		    alpha * raw_offset_cm
		    + (1.0 - alpha) * filtered_offset_cm_;
	}

	result.valid = true;
	result.calibrated = true;
	result.confidence =
	    std::clamp(best_circularity, 0.0, 1.0);
	result.offset_mm = static_cast<int>(
	    std::lround(filtered_offset_cm_ * 10.0));
	result.error_code.clear();

	return result;
}

// ────────────────────────────────────────────────────────────
// 调试绘制（分屏：原始标注 | track mask | ball binary | 状态）
// ────────────────────────────────────────────────────────────

void VisionProcessor::drawDebugInfo(
    cv::Mat& frame, const VisionResult& result) noexcept
{
	try
	{
		if(frame.empty())
		{
			ETEST_LOG_WARN("VISION",
			               "drawDebugInfo received empty frame");
			return;
		}

		if(result.target_type == "BALL")
		{
			drawBallDebugInfo(frame, result);
			return;
		}

		const cv::Point image_center(frame.cols / 2,
		                             frame.rows / 2);
		cv::drawMarker(frame, image_center,
		               cv::Scalar(255, 0, 0),
		               cv::MARKER_CROSS, 20, 2);

		if(!result.valid)
		{
			cv::putText(frame, "Target: LOST",
			            cv::Point(20, 30),
			            cv::FONT_HERSHEY_SIMPLEX, 0.7,
			            cv::Scalar(0, 0, 255), 2);
			return;
		}

		const cv::Point target(
		    static_cast<int>(result.x),
		    static_cast<int>(result.y));
		cv::circle(frame, target, 8,
		           cv::Scalar(0, 255, 0), 2);
		cv::line(frame, image_center, target,
		         cv::Scalar(0, 255, 0), 2);
		cv::putText(frame, "Target: FOUND",
		            cv::Point(20, 30),
		            cv::FONT_HERSHEY_SIMPLEX, 0.7,
		            cv::Scalar(0, 255, 0), 2);
	}
	catch(const cv::Exception& error)
	{
		ETEST_LOG_ERROR("VISION",
		                std::string("draw exception: ")
		                    + error.what());
	}
	catch(...)
	{
		ETEST_LOG_ERROR("VISION", "unknown draw exception");
	}
}

void VisionProcessor::drawBallDebugInfo(
    cv::Mat& frame, const VisionResult& result) noexcept
{
	try
	{
		const auto& ball = config_.ball;
		const int w = frame.cols;
		const int h = frame.rows;

		cv::Mat canvas(
		    cv::Size(w * 2, h),
		    frame.type(),
		    cv::Scalar(0, 0, 0));

		// 左上：原始标注帧
		cv::Mat top_left = canvas(
		    cv::Rect(0, 0, w, h / 2));
		cv::Mat src_top =
		    frame(cv::Rect(0, 0, w, h / 2));
		src_top.copyTo(top_left);

		if(track_locked_)
		{
			const cv::Rect& roi =
			    locked_track_.bounding_roi;
			cv::rectangle(
			    top_left,
			    makeInnerRoi(roi, frame.size()),
			    cv::Scalar(0, 255, 0), 1);

			cv::line(
			    top_left,
			    cv::Point(
			        static_cast<int>(
			            locked_track_.axis_p1.x),
			        static_cast<int>(
			            locked_track_.axis_p1.y)),
			    cv::Point(
			        static_cast<int>(
			            locked_track_.axis_p2.x),
			        static_cast<int>(
			            locked_track_.axis_p2.y)),
			    cv::Scalar(0, 255, 255), 1);
		}

		if(result.valid && result.calibrated)
		{
			cv::circle(
			    top_left,
			    cv::Point(
			        static_cast<int>(result.x),
			        static_cast<int>(result.y)),
			    6, cv::Scalar(0, 0, 255), -1);

			const int conf_pct =
			    static_cast<int>(std::lround(
			        result.confidence * 100.0));
			std::string label =
			    "BALL "
			    + std::to_string(result.offset_mm)
			    + "mm "
			    + std::to_string(conf_pct) + "%";
			cv::putText(top_left, label,
			            cv::Point(5, 15),
			            cv::FONT_HERSHEY_SIMPLEX,
			            0.5,
			            cv::Scalar(0, 255, 0), 1);
		}
		else
		{
			const char* state_text =
			    "FIND_TRACK";
			switch(ball_state_)
			{
			case BallState::CALIBRATE_ZERO:
				state_text = "CALIB";
				break;
			case BallState::TRACK_BALL:
				state_text =
				    result.error_code
				            == "BALL_LOST"
				        ? "LOST"
				        : "TRACK";
				break;
			case BallState::REACQUIRE_BALL:
				state_text = "REACQ";
				break;
			default:
				break;
			}

			cv::putText(top_left,
			            std::string(state_text),
			            cv::Point(5, 15),
			            cv::FONT_HERSHEY_SIMPLEX,
			            0.5,
			            cv::Scalar(0, 165, 255), 1);
		}

		// 右上：track mask
		cv::Mat top_right = canvas(
		    cv::Rect(w, 0, w, h / 2));
		if(!debug_track_mask_.empty())
		{
			cv::Mat mask_top = debug_track_mask_(
			    cv::Rect(
			        0, 0,
			        std::min(debug_track_mask_.cols,
			                 w),
			        std::min(debug_track_mask_.rows,
			                 h / 2)));
			cv::Mat mask_color;
			cv::cvtColor(mask_top, mask_color,
			             cv::COLOR_GRAY2BGR);
			mask_color.copyTo(
			    top_right(
			        cv::Rect(0, 0, mask_color.cols,
			                 mask_color.rows)));
		}

		// 左下：ball binary
		cv::Mat bottom_left = canvas(
		    cv::Rect(0, h / 2, w, h / 2));
		if(!debug_ball_binary_.empty())
		{
			cv::Mat bin_half = debug_ball_binary_(
			    cv::Rect(
			        0, 0,
			        std::min(debug_ball_binary_.cols,
			                 w),
			        std::min(debug_ball_binary_.rows,
			                 h / 2)));
			cv::Mat bin_color;
			cv::cvtColor(bin_half, bin_color,
			             cv::COLOR_GRAY2BGR);
			bin_color.copyTo(
			    bottom_left(
			        cv::Rect(0, 0, bin_color.cols,
			                 bin_color.rows)));
		}

		// 右下：状态文本
		cv::Mat bottom_right = canvas(
		    cv::Rect(w, h / 2, w, h / 2));
		std::string status_text;
		if(result.valid && result.calibrated)
		{
			status_text = "OK offset="
			    + std::to_string(result.offset_mm)
			    + "mm";
		}
		else if(result.error_code
		         == "ZERO_CALIBRATING")
		{
			status_text = "CALIBRATING "
			    + std::to_string(
			          zero_buffer_.size())
			    + "/"
			    + std::to_string(
			          ball.zero_samples);
		}
		else
		{
			status_text = result.error_code;
		}

		cv::putText(bottom_right, status_text,
		            cv::Point(5, 20),
		            cv::FONT_HERSHEY_SIMPLEX,
		            0.5,
		            cv::Scalar(200, 200, 200), 1);

		cv::resize(canvas, frame,
		           cv::Size(w, h), 0, 0,
		           cv::INTER_LINEAR);
	}
	catch(const cv::Exception& error)
	{
		ETEST_LOG_ERROR(
		    "VISION",
		    std::string("ball draw exception: ")
		        + error.what());
	}
	catch(...)
	{
		ETEST_LOG_ERROR(
		    "VISION", "unknown ball draw exception");
	}
}

// ────────────────────────────────────────────────────────────
// 神经网络检测
// ────────────────────────────────────────────────────────────

bool VisionProcessor::loadNnModel(
    const std::string& onnx_path,
    const std::string& class_names_path,
    double confidence_threshold,
    double nms_threshold) noexcept
{
	try
	{
		nn_net_ = cv::dnn::readNetFromONNX(onnx_path);
		if(nn_net_.empty())
		{
			ETEST_LOG_ERROR(
			    "VISION_NN",
			    "failed to load ONNX model: "
			        + onnx_path);
			nn_loaded_ = false;
			return false;
		}

		nn_confidence_threshold_ =
		    confidence_threshold;
		nn_nms_threshold_ = nms_threshold;
		nn_class_names_.clear();

		if(!class_names_path.empty())
		{
			std::ifstream class_file(
			    class_names_path);
			if(class_file.is_open())
			{
				std::string name;
				while(std::getline(class_file,
				                   name))
				{
					if(!name.empty())
						nn_class_names_.push_back(
						    name);
				}
			}
		}

		nn_output_names_ =
		    nn_net_.getUnconnectedOutLayersNames();
		nn_loaded_ = true;
		return true;
	}
	catch(const cv::Exception& error)
	{
		ETEST_LOG_ERROR(
		    "VISION_NN",
		    std::string("failed to load ONNX: ")
		        + error.what());
	}
	catch(const std::exception& error)
	{
		ETEST_LOG_ERROR(
		    "VISION_NN",
		    std::string("failed to load ONNX: ")
		        + error.what());
	}
	catch(...)
	{
		ETEST_LOG_ERROR(
		    "VISION_NN",
		    "unknown ONNX load exception");
	}

	nn_loaded_ = false;
	return false;
}

cv::Mat VisionProcessor::detectNn(
    const cv::Mat& frame) noexcept
{
	try
	{
		if(!nn_loaded_ || frame.empty())
			return frame.clone();

		constexpr int in_w = 640;
		constexpr int in_h = 640;

		cv::Mat blob = cv::dnn::blobFromImage(
		    frame, 1.0 / 255.0,
		    cv::Size(in_w, in_h),
		    cv::Scalar(), true, false);

		nn_net_.setInput(blob);

		std::vector<cv::Mat> outputs;
		nn_net_.forward(outputs, nn_output_names_);

		const float fw =
		    static_cast<float>(frame.cols);
		const float fh =
		    static_cast<float>(frame.rows);
		const float xs = fw / in_w;
		const float ys = fh / in_h;

		std::vector<cv::Rect> boxes;
		std::vector<float> confidences;
		std::vector<int> class_ids;

		for(const auto& output: outputs)
		{
			const auto* data =
			    reinterpret_cast<const float*>(
			        output.data);
			const int rows = output.size[1];
			const int cols = output.size[2];

			for(int r = 0; r < rows; ++r)
			{
				const float* rd =
				    data + r * cols;
				const float obj = rd[4];
				if(obj < nn_confidence_threshold_)
					continue;

				float max_cls = 0.0F;
				int best_cls = 0;
				for(int c = 0; c < 80; ++c)
				{
					const float cf =
					    rd[5 + c];
					if(cf > max_cls)
					{
						max_cls = cf;
						best_cls = c;
					}
				}

				const float fc =
				    obj * max_cls;
				if(fc < nn_confidence_threshold_)
					continue;

				const float cx = rd[0];
				const float cy = rd[1];
				const float w2 = rd[2];
				const float h2 = rd[3];

				boxes.emplace_back(
				    static_cast<int>(
				        (cx - 0.5F * w2) * xs),
				    static_cast<int>(
				        (cy - 0.5F * h2) * ys),
				    static_cast<int>(w2 * xs),
				    static_cast<int>(h2 * ys));
				confidences.push_back(fc);
				class_ids.push_back(best_cls);
			}
		}

		std::vector<int> nms_idx;
		cv::dnn::NMSBoxes(
		    boxes, confidences,
		    nn_confidence_threshold_,
		    nn_nms_threshold_, nms_idx);

		last_detections_.clear();
		cv::Mat result = frame.clone();

		for(int idx: nms_idx)
		{
			const cv::Rect& box = boxes[idx];
			const int cid = class_ids[idx];
			const float conf = confidences[idx];

			std::string name =
			    "class_" + std::to_string(cid);
			if(cid >= 0
			   && static_cast<std::size_t>(cid)
			       < nn_class_names_.size())
			{
				name = nn_class_names_[cid];
			}

			last_detections_.push_back(
			    {name, conf, box.x, box.y, box.x,
			     box.y + box.height,
			     box.x + box.width,
			     box.y + box.height,
			     box.x + box.width, box.y});

			cv::rectangle(
			    result, box,
			    cv::Scalar(0, 255, 0), 2);
			cv::putText(
			    result,
			    name + " "
			        + std::to_string(
			            static_cast<int>(
			                conf * 100))
			        + "%",
			    cv::Point(box.x, box.y - 5),
			    cv::FONT_HERSHEY_SIMPLEX, 0.5,
			    cv::Scalar(0, 255, 0), 1);
		}

		return result;
	}
	catch(const cv::Exception& error)
	{
		ETEST_LOG_ERROR(
		    "VISION_NN",
		    std::string("detectNn: ")
		        + error.what());
		return frame.clone();
	}
	catch(const std::exception& error)
	{
		ETEST_LOG_ERROR(
		    "VISION_NN",
		    std::string("detectNn: ")
		        + error.what());
		return frame.clone();
	}
	catch(...)
	{
		ETEST_LOG_ERROR(
		    "VISION_NN",
		    "unknown detectNn exception");
		return frame.clone();
	}
}

bool VisionProcessor::isNnLoaded() const noexcept
{
	return nn_loaded_;
}

const std::vector<DetectionInfo>&
VisionProcessor::getLastDetections() const noexcept
{
	return last_detections_;
}

} // namespace etest::vision