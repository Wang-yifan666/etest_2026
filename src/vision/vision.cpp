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
			result.timestamp_ms =
			    static_cast<std::int64_t>(timestamp_ms);
			result.target_type =
			    (mode == VisionMode::Ball) ? "BALL" : "";
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

		cv::Mat mask1, mask2;
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
	// 棕色管道检测（搜索区域 + 横向形态学 + 角度过滤 + 填充率）
	// ────────────────────────────────────────────────────────────

	TrackResult VisionProcessor::detectBrownPipe(
	    const cv::Mat& frame) noexcept
	{
		TrackResult result;

		try
		{
			if(frame.empty())
			{
				ETEST_LOG_ERROR("PIPE",
				                "detectBrownPipe received empty frame");
				return result;
			}

			const auto& ball = config_.ball;

			// 1) 限定搜索区域
			cv::Rect search_roi(
			    ball.track_search_roi_x, ball.track_search_roi_y,
			    ball.track_search_roi_w, ball.track_search_roi_h);
			search_roi &= cv::Rect(0, 0, frame.cols, frame.rows);

			if(search_roi.empty() || search_roi.area() < 100)
			{
				debug_track_mask_ =
				    cv::Mat::zeros(frame.size(), CV_8UC1);
				return result;
			}

			const cv::Mat search_image = frame(search_roi);

			// 2) HSV 棕色阈值
			cv::Mat hsv;
			cv::cvtColor(search_image, hsv, cv::COLOR_BGR2HSV);

			cv::Mat brown_mask;
			cv::inRange(hsv,
			            cv::Scalar(ball.brown_h_min, ball.brown_s_min,
			                       ball.brown_v_min),
			            cv::Scalar(ball.brown_h_max, ball.brown_s_max,
			                       ball.brown_v_max),
			            brown_mask);

			// 3) 矩形闭运算连接管道区域
			const cv::Mat close_kernel = cv::getStructuringElement(
			    cv::MORPH_RECT,
			    cv::Size(ball.pipe_close_kernel_w,
			             ball.pipe_close_kernel_h));
			cv::morphologyEx(brown_mask, brown_mask, cv::MORPH_CLOSE,
			                 close_kernel);

			// 4) 小尺度开运算去除噪点
			const cv::Mat open_kernel = cv::getStructuringElement(
			    cv::MORPH_ELLIPSE,
			    cv::Size(ball.pipe_open_kernel, ball.pipe_open_kernel));
			cv::morphologyEx(brown_mask, brown_mask, cv::MORPH_OPEN,
			                 open_kernel);

			// 将局部 mask 放回全图坐标供调试显示
			debug_track_mask_ = cv::Mat::zeros(frame.size(), CV_8UC1);
			brown_mask.copyTo(debug_track_mask_(search_roi));

			// 5) 轮廓筛选：面积比例 + 长宽比 + 横向角度 + 填充率
			std::vector<std::vector<cv::Point>> contours;
			cv::findContours(brown_mask, contours, cv::RETR_EXTERNAL,
			                 cv::CHAIN_APPROX_SIMPLE);

			const double image_area =
			    static_cast<double>(frame.cols) * frame.rows;

			double best_score = -1.0;
			cv::RotatedRect best_rect;

			for(const auto& contour: contours)
			{
				const double area = cv::contourArea(contour);
				if(area < image_area * ball.pipe_min_area_ratio)
					continue;

				const cv::RotatedRect rect = cv::minAreaRect(contour);

				const double long_side =
				    std::max(rect.size.width, rect.size.height);
				const double short_side =
				    std::min(rect.size.width, rect.size.height);

				if(short_side < 5.0)
					continue;

				const double aspect = long_side / short_side;
				if(aspect < ball.pipe_min_aspect_ratio)
					continue;

				// 横向角度过滤
				cv::Point2f pts[4];
				rect.points(pts);

				const cv::Point2f e1 = pts[1] - pts[0];
				const cv::Point2f e2 = pts[2] - pts[1];
				const cv::Point2f long_edge =
				    cv::norm(e1) >= cv::norm(e2) ? e1 : e2;

				const double angle_deg =
				    std::atan2(long_edge.y, long_edge.x) * 180.0
				    / CV_PI;

				const double horizontal_deviation =
				    std::min(std::abs(angle_deg),
				             std::abs(180.0 - std::abs(angle_deg)));

				if(horizontal_deviation
				   > ball.pipe_horizontal_angle_max)
					continue;

				// 矩形填充率
				const double rect_area =
				    rect.size.width * rect.size.height;
				const double fill_ratio =
				    (rect_area > 0.0) ? area / rect_area : 0.0;

				if(fill_ratio < ball.pipe_min_fill_ratio)
					continue;

				// 矩形尺寸必须足以容纳钢球
				const double ball_radius_px =
				    std::sqrt(ball.max_area / CV_PI) * 2.0;
				if(short_side < ball_radius_px)
					continue;

				// 综合评分：面积 × 长宽比 × 填充率
				const double score =
				    area * std::min(aspect, 20.0) * fill_ratio;

				if(score > best_score)
				{
					best_score = score;
					best_rect = rect;
				}
			}

			if(best_score < 0.0)
				return result;

			// 6) 坐标加回全图
			best_rect.center.x += search_roi.x;
			best_rect.center.y += search_roi.y;

			result.valid = true;
			result.rect = best_rect;
			result.bounding_roi =
			    cv::Rect(search_roi.x + best_rect.boundingRect().x
			                 - search_roi.x,
			             search_roi.y + best_rect.boundingRect().y
			                 - search_roi.y,
			             best_rect.boundingRect().width,
			             best_rect.boundingRect().height);
			result.bounding_roi &=
			    cv::Rect(0, 0, frame.cols, frame.rows);

			result.confidence =
			    std::clamp(best_score / (image_area * 20.0), 0.0, 1.0);

			// 提取轴线（四点→短边中点连线）
			cv::Point2f points[4];
			best_rect.points(points);

			const double edge01 = cv::norm(points[1] - points[0]);
			const double edge12 = cv::norm(points[2] - points[1]);

			if(edge01 >= edge12)
			{
				result.axis_p1 = (points[0] + points[3]) * 0.5F;
				result.axis_p2 = (points[1] + points[2]) * 0.5F;
			}
			else
			{
				result.axis_p1 = (points[0] + points[1]) * 0.5F;
				result.axis_p2 = (points[2] + points[3]) * 0.5F;
			}

			return result;
		}
		catch(const cv::Exception& error)
		{
			ETEST_LOG_ERROR(
			    "PIPE",
			    std::string("OpenCV exception: ") + error.what());
		}
		catch(const std::exception& error)
		{
			ETEST_LOG_ERROR("PIPE",
			                std::string("exception: ") + error.what());
		}
		catch(...)
		{
			ETEST_LOG_ERROR("PIPE", "unknown exception");
		}

		return result;
	}

	bool VisionProcessor::isTrackSimilar(
	    const TrackResult& a, const TrackResult& b) const noexcept
	{
		if(!a.valid || !b.valid)
			return false;

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
	    const cv::Rect& track_roi, const cv::Size& work_size) noexcept
	{
		const int margin_x =
		    std::max(1, static_cast<int>(track_roi.width * 0.03));
		const int margin_y =
		    std::max(1, static_cast<int>(track_roi.height * 0.15));

		// 先确保最小尺寸，再与工作图像求交，避免求交后强制改大导致越界
		cv::Rect inner(track_roi.x + margin_x, track_roi.y + margin_y,
		               std::max(10, track_roi.width - margin_x * 2),
		               std::max(10, track_roi.height - margin_y * 2));

		inner &= cv::Rect(0, 0, work_size.width, work_size.height);

		if(inner.width < 10 || inner.height < 10)
			return cv::Rect();

		return inner;
	}

	// ────────────────────────────────────────────────────────────
	// Ball 检测（统一入口，内含状态机）
	// ────────────────────────────────────────────────────────────

	VisionResult VisionProcessor::detectBall(const cv::Mat& frame)
	{
		VisionResult result;
		result.target_type = "BALL";

		// 0) 统一到可配置工作分辨率
		const auto& ball = config_.ball;
		const int kWorkWidth = ball.work_width;
		const int kWorkHeight = ball.work_height;

		cv::Mat work_frame;

		if(frame.cols != kWorkWidth || frame.rows != kWorkHeight)
		{
			if(!ball_resolution_reported_)
			{
				ETEST_LOG_INFO("VISION",
				               "resizing input from "
				                   + std::to_string(frame.cols) + "x"
				                   + std::to_string(frame.rows)
				                   + " to work resolution "
				                   + std::to_string(kWorkWidth) + "x"
				                   + std::to_string(kWorkHeight));
				ball_resolution_reported_ = true;
			}

			cv::resize(frame, work_frame,
			           cv::Size(kWorkWidth, kWorkHeight), 0.0, 0.0,
			           cv::INTER_LINEAR);
		}
		else
		{
			work_frame = frame;
		}

		// ── 1) 管道检测 → 维护锁定状态 ──
		if(!track_locked_)
		{
			const TrackResult track = detectBrownPipe(work_frame);

			if(track.valid)
			{
				if(isTrackSimilar(track, locked_track_))
				{
					++track_stable_count_;
					if(track_stable_count_ >= ball.pipe_stable_frames)
					{
						track_locked_ = true;
						locked_track_ = track;
						track_lost_frame_count_ = 0;
						warp_locked_ = false;
						ETEST_LOG_INFO("VISION",
						               "pipe geometry locked");
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
			const TrackResult track = detectBrownPipe(work_frame);

			if(!track.valid || !isTrackSimilar(track, locked_track_))
			{
				if(track_lost_frame_count_ == 0)
					ETEST_LOG_WARN("VISION", "pipe lost");
				++track_lost_frame_count_;

				if(track_lost_frame_count_
				   >= ball.pipe_lost_timeout_frames)
				{
					track_locked_ = false;
					track_stable_count_ = 0;
					track_lost_frame_count_ = 0;
					warp_locked_ = false;
					ball_state_ = BallState::FIND_TRACK;
					zero_locked_ = false;
					zero_buffer_.clear();
					ball_filter_initialized_ = false;
					ETEST_LOG_WARN(
					    "VISION",
					    "pipe lost timeout; resetting to FIND_TRACK");
				}
			}
			else
			{
				if(track_lost_frame_count_ > 0)
					ETEST_LOG_INFO(
					    "VISION",
					    "pipe recovered; locked geometry kept");
				track_lost_frame_count_ = 0;
			}
		}

		if(!track_locked_)
		{
			result.error_code = "PIPE_NOT_STABLE";
			return result;
		}

		// ── 2) 透视展开管道 ROI ──
		if(!warp_locked_)
		{
			const auto& rect = locked_track_.rect;

			cv::Point2f pts[4];
			rect.points(pts);

			// 四点排序：左上、右上、右下、左下
			std::sort(pts, pts + 4,
			          [](const cv::Point2f& a, const cv::Point2f& b) {
				          return (a.y < b.y)
				              || (a.y == b.y && a.x < b.x);
			          });

			cv::Point2f top_left, top_right;
			if(pts[0].x <= pts[1].x)
			{
				top_left = pts[0];
				top_right = pts[1];
			}
			else
			{
				top_left = pts[1];
				top_right = pts[0];
			}

			cv::Point2f bottom_left, bottom_right;
			if(pts[2].x <= pts[3].x)
			{
				bottom_left = pts[2];
				bottom_right = pts[3];
			}
			else
			{
				bottom_left = pts[3];
				bottom_right = pts[2];
			}

			const cv::Point2f src[4] = {top_left, top_right,
			                            bottom_right, bottom_left};

			// 锁定左右方向
			const double top_dx = top_right.x - top_left.x;
			warp_direction_ = (top_dx >= 0.0) ? 1 : -1;

			const int ww = ball.pipe_warp_width;
			const int wh = ball.pipe_warp_height;
			const cv::Point2f dst[4] = {
			    cv::Point2f(0.0F, 0.0F),
			    cv::Point2f(static_cast<float>(ww - 1), 0.0F),
			    cv::Point2f(static_cast<float>(ww - 1),
			                static_cast<float>(wh - 1)),
			    cv::Point2f(0.0F, static_cast<float>(wh - 1))};

			warp_matrix_ = cv::getPerspectiveTransform(src, dst);

			if(warp_matrix_.empty()
			   || std::isnan(warp_matrix_.at<double>(0, 0)))
			{
				ETEST_LOG_ERROR("VISION",
				                "perspective transform failed");
				result.error_code = "PIPE_WARP_FAILED";
				return result;
			}

			warp_locked_ = true;
		}

		// ── 3) 执行透视变换 ──
		cv::Mat warped;
		cv::warpPerspective(
		    work_frame, warped, warp_matrix_,
		    cv::Size(ball.pipe_warp_width, ball.pipe_warp_height),
		    cv::INTER_LINEAR);
		debug_warped_pipe_ = warped.clone();

		// 定义内部有效区域（排除管壁边缘）
		const int margin_x = static_cast<int>(
		    ball.pipe_warp_width * ball.pipe_inner_margin_x_ratio);
		const int margin_y = static_cast<int>(
		    ball.pipe_warp_height * ball.pipe_inner_margin_y_ratio);
		cv::Rect inner_roi(
		    margin_x, margin_y,
		    std::max(1, ball.pipe_warp_width - 2 * margin_x),
		    std::max(1, ball.pipe_warp_height - 2 * margin_y));
		inner_roi &= cv::Rect(0, 0, warped.cols, warped.rows);

		if(inner_roi.empty() || inner_roi.width < 10
		   || inner_roi.height < 5)
		{
			result.error_code = "PIPE_ROI_INVALID";
			return result;
		}

		const cv::Mat roi_img = warped(inner_roi);

		// ── 4) 灰度 + 高斯滤波 ──
		cv::Mat gray;
		if(roi_img.channels() == 3)
			cv::cvtColor(roi_img, gray, cv::COLOR_BGR2GRAY);
		else
			gray = roi_img.clone();

		cv::GaussianBlur(gray, gray, cv::Size(5, 5), 0.0);

		// ── 5) 黑帽+顶帽：适应球体可能比背景亮或暗 ──
		const cv::Mat se = cv::getStructuringElement(
		    cv::MORPH_ELLIPSE,
		    cv::Size(ball.bg_kernel, ball.bg_kernel));

		cv::Mat blackhat;
		cv::morphologyEx(gray, blackhat, cv::MORPH_BLACKHAT, se);

		cv::Mat tophat;
		cv::morphologyEx(gray, tophat, cv::MORPH_TOPHAT, se);

		cv::Mat combined = cv::max(blackhat, tophat);

		// ── 6) 二值化 ──
		cv::Mat binary;
		cv::threshold(combined, binary, ball.threshold, 255,
		              cv::THRESH_BINARY);

		const cv::Mat morph_kernel = cv::getStructuringElement(
		    cv::MORPH_ELLIPSE,
		    cv::Size(ball.morph_kernel, ball.morph_kernel));
		cv::morphologyEx(binary, binary, cv::MORPH_CLOSE, morph_kernel);
		cv::morphologyEx(binary, binary, cv::MORPH_OPEN, morph_kernel);

		debug_ball_binary_ = binary.clone();

		// ── 7) 轮廓筛选 ──
		std::vector<std::vector<cv::Point>> contours;
		cv::findContours(binary, contours, cv::RETR_EXTERNAL,
		                 cv::CHAIN_APPROX_SIMPLE);

		// 管道中心线（水平展开后中心线 y=warp_height/2）
		const double centerline_y =
		    ball.pipe_warp_height * 0.5 - margin_y;

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

			const double perimeter = cv::arcLength(contour, true);
			if(perimeter < 1.0)
				continue;

			const double circularity =
			    4.0 * CV_PI * area / (perimeter * perimeter);
			if(circularity < ball.min_circularity)
				continue;

			const cv::Rect box = cv::boundingRect(contour);
			const double aspect = static_cast<double>(box.width)
			    / std::max(1, box.height);
			if(aspect < ball.ball_min_aspect
			   || aspect > ball.ball_max_aspect)
				continue;

			const cv::Moments moments = cv::moments(contour);
			if(std::abs(moments.m00) < 1e-6)
				continue;

			const cv::Point2f center_in_roi(
			    static_cast<float>(moments.m10 / moments.m00),
			    static_cast<float>(moments.m01 / moments.m00));

			// 局部对比度：轮廓区域 vs 外围环的平均灰度差
			cv::Mat mask = cv::Mat::zeros(binary.size(), CV_8UC1);
			cv::drawContours(
			    mask, std::vector<std::vector<cv::Point>>{contour}, -1,
			    255, -1);

			cv::Mat dilated_mask;
			const cv::Mat dilate_kernel = cv::getStructuringElement(
			    cv::MORPH_ELLIPSE, cv::Size(11, 11));
			cv::dilate(mask, dilated_mask, dilate_kernel);
			cv::Mat outer_ring = dilated_mask - mask;

			const double inner_mean = cv::mean(gray, mask)[0];
			const double outer_mean = cv::mean(gray, outer_ring)[0];
			const double local_contrast =
			    std::abs(inner_mean - outer_mean);

			if(local_contrast < ball.ball_min_local_contrast)
				continue;

			// 到中心线距离
			const double centerline_distance =
			    std::abs(center_in_roi.y - centerline_y);
			if(centerline_distance
			   > ball.ball_max_centerline_distance_px)
				continue;

			// 位置跳变检查
			double jump_distance = 0.0;
			if(!in_reacquire && has_last_ball_center_)
			{
				jump_distance =
				    cv::norm(center_in_roi - last_ball_center_);
				if(jump_distance > ball.max_jump_px)
					continue;
			}

			// 评分：圆度 + 对比度 - 中心线距离 - 跳变
			const double score = circularity * 100.0
			    + local_contrast * 2.0 - centerline_distance * 1.5
			    - jump_distance * 0.2;

			if(score > best_score)
			{
				best_score = score;
				best_center = center_in_roi;
				best_circularity = circularity;
			}
		}

		// ── 8) 无候选 → 状态机处理 ──
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
				const auto now = std::chrono::steady_clock::now();
				const auto elapsed = std::chrono::duration_cast<
				    std::chrono::milliseconds>(
				    now - last_ball_lost_log_time_);
				if(elapsed.count() >= 1000)
				{
					ETEST_LOG_WARN(
					    "VISION",
					    "Ball remains lost, frames="
					        + std::to_string(ball_lost_frame_count_));
					last_ball_lost_log_time_ = now;
				}
			}

			if(ball_lost_frame_count_
			   >= ball.reacquire_after_lost_frames)
			{
				if(ball_state_ == BallState::TRACK_BALL
				   || ball_state_ == BallState::CALIBRATE_ZERO)
				{
					ball_state_ = BallState::REACQUIRE_BALL;
					has_last_ball_center_ = false;
					ball_filter_initialized_ = false;
				}
			}

			if(!zero_locked_)
				zero_buffer_.clear();

			result.error_code = "BALL_NOT_FOUND";
			return result;
		}

		// ── 9) 检测成功 → 校正 ──
		ball_lost_frame_count_ = 0;

		if(ball_lost_)
		{
			ball_lost_ = false;
			ETEST_LOG_INFO("VISION", "Ball recovered");
		}

		has_last_ball_center_ = true;
		last_ball_center_ = best_center;

		const double axis_position_px = best_center.x + inner_roi.x;

		// 外推 result.x/y 到工作帧坐标（调试用）
		result.x =
		    best_center.x + inner_roi.x + locked_track_.bounding_roi.x;
		result.y =
		    best_center.y + inner_roi.y + locked_track_.bounding_roi.y;

		if(ball_state_ == BallState::REACQUIRE_BALL)
		{
			ball_state_ = BallState::TRACK_BALL;
			ETEST_LOG_INFO("VISION", "Ball reacquired; resuming TRACK");
		}

		// ── 10) 零点校准 ──
		if(!zero_locked_)
		{
			if(ball_state_ != BallState::CALIBRATE_ZERO
			   && ball_state_ != BallState::TRACK_BALL)
			{
				ball_state_ = BallState::CALIBRATE_ZERO;
			}

			if(ball.zero_mode == "fixed")
			{
				zero_position_px_ = ball.zero_position_px;
				zero_locked_ = true;
				ball_state_ = BallState::TRACK_BALL;
				ETEST_LOG_INFO("VISION",
				               "Ball zero locked (fixed): "
				                   + std::to_string(zero_position_px_)
				                   + " px");
			}
			else
			{
				if(!zero_buffer_.empty())
				{
					const double last = zero_buffer_.back();
					if(std::abs(axis_position_px - last)
					   > ball.max_jump_px)
						zero_buffer_.clear();
				}

				zero_buffer_.push_back(axis_position_px);

				while(static_cast<int>(zero_buffer_.size())
				      > ball.zero_samples)
					zero_buffer_.pop_front();

				if(static_cast<int>(zero_buffer_.size())
				   >= ball.zero_samples)
				{
					std::vector<double> values(zero_buffer_.begin(),
					                           zero_buffer_.end());
					std::sort(values.begin(), values.end());

					const double median = values[values.size() / 2];
					const double range = values.back() - values.front();

					if(range <= ball.zero_range_px)
					{
						zero_position_px_ = median;
						zero_locked_ = true;
						ball_state_ = BallState::TRACK_BALL;
						ETEST_LOG_INFO("VISION",
						               "Ball zero calibrated: median="
						                   + std::to_string(median)
						                   + " px, range="
						                   + std::to_string(range));
					}
				}
			}
		}

		if(!zero_locked_)
		{
			result.error_code = "ZERO_CALIBRATING";
			return result;
		}

		// ── 11) 毫米换算 + 低通滤波 ──
		const double effective_warp_width = ball.pipe_warp_width
		    * (1.0 - 2.0 * ball.pipe_inner_margin_x_ratio);
		const double mm_per_warp_pixel = effective_warp_width > 0.0
		    ? ball.pipe_length_mm / effective_warp_width
		    : ball.pipe_length_mm / ball.pipe_warp_width;

		const double raw_offset_mm =
		    (axis_position_px - zero_position_px_) * mm_per_warp_pixel;

		if(!ball_filter_initialized_)
		{
			filtered_offset_cm_ = raw_offset_mm / 10.0;
			ball_filter_initialized_ = true;
		}
		else
		{
			const double alpha =
			    std::clamp(ball.filter_alpha, 0.01, 1.0);
			filtered_offset_cm_ = alpha * raw_offset_mm / 10.0
			    + (1.0 - alpha) * filtered_offset_cm_;
		}

		result.valid = true;
		result.calibrated = true;
		result.confidence = std::clamp(best_circularity, 0.0, 1.0);
		result.offset_mm =
		    static_cast<int>(std::lround(filtered_offset_cm_ * 10.0));
		result.error_code.clear();

		return result;
	}

	// ────────────────────────────────────────────────────────────
	// 调试绘制（分屏）
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
			cv::drawMarker(frame, image_center, cv::Scalar(255, 0, 0),
			               cv::MARKER_CROSS, 20, 2);

			if(!result.valid)
			{
				cv::putText(frame, "Target: LOST", cv::Point(20, 30),
				            cv::FONT_HERSHEY_SIMPLEX, 0.7,
				            cv::Scalar(0, 0, 255), 2);
				return;
			}

			const cv::Point target(static_cast<int>(result.x),
			                       static_cast<int>(result.y));
			cv::circle(frame, target, 8, cv::Scalar(0, 255, 0), 2);
			cv::line(frame, image_center, target, cv::Scalar(0, 255, 0),
			         2);
			cv::putText(frame, "Target: FOUND", cv::Point(20, 30),
			            cv::FONT_HERSHEY_SIMPLEX, 0.7,
			            cv::Scalar(0, 255, 0), 2);
		}
		catch(const cv::Exception& error)
		{
			ETEST_LOG_ERROR(
			    "VISION",
			    std::string("draw exception: ") + error.what());
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
			const int kW = ball.work_width;
			const int kH = ball.work_height;

			cv::Mat work_view;
			if(frame.cols != kW || frame.rows != kH)
			{
				cv::resize(frame, work_view, cv::Size(kW, kH), 0.0, 0.0,
				           cv::INTER_LINEAR);
			}
			else
			{
				work_view = frame.clone();
			}

			const cv::Size work_size(kW, kH);

			// canvas: 2×2 宫格，使用 display_w = max(kW/2, pipe_warp_width)
			// 以适应展开管道宽度
			const int disp_w =
			    std::max(kW / 2, ball.pipe_warp_width / 2);
			const int disp_h = kH / 2;
			cv::Mat canvas(cv::Size(kW + disp_w, kH), work_view.type(),
			               cv::Scalar(0, 0, 0));

			// ── 左上：完整标注帧（旋转矩形 + 球位置）──
			cv::Mat top_left = canvas(cv::Rect(0, 0, kW, kH / 2));
			cv::Mat annotated = work_view.clone();

			if(track_locked_)
			{
				// 绘制锁定的旋转矩形
				cv::Point2f rpts[4];
				locked_track_.rect.points(rpts);
				for(int i = 0; i < 4; ++i)
					cv::line(annotated,
					         cv::Point(static_cast<int>(rpts[i].x),
					                   static_cast<int>(rpts[i].y)),
					         cv::Point(
					             static_cast<int>(rpts[(i + 1) % 4].x),
					             static_cast<int>(rpts[(i + 1) % 4].y)),
					         cv::Scalar(0, 255, 255), 2);

				// 管道轴线（黄色）
				cv::line(annotated,
				         cv::Point(
				             static_cast<int>(locked_track_.axis_p1.x),
				             static_cast<int>(locked_track_.axis_p1.y)),
				         cv::Point(
				             static_cast<int>(locked_track_.axis_p2.x),
				             static_cast<int>(locked_track_.axis_p2.y)),
				         cv::Scalar(0, 255, 255), 1);
			}

			if(result.valid && result.calibrated)
			{
				cv::circle(annotated,
				           cv::Point(static_cast<int>(result.x),
				                     static_cast<int>(result.y)),
				           8, cv::Scalar(0, 0, 255), -1);

				const int conf_pct = static_cast<int>(
				    std::lround(result.confidence * 100.0));
				std::string label = "BALL "
				    + std::to_string(result.offset_mm) + "mm "
				    + std::to_string(conf_pct) + "%";
				cv::putText(annotated, label, cv::Point(5, 20),
				            cv::FONT_HERSHEY_SIMPLEX, 0.7,
				            cv::Scalar(0, 255, 0), 2);
			}
			else
			{
				const char* state_text = "FIND_TRACK";
				switch(ball_state_)
				{
				case BallState::CALIBRATE_ZERO:
					state_text = "CALIB";
					break;
				case BallState::TRACK_BALL:
					state_text = "TRACK";
					break;
				case BallState::REACQUIRE_BALL:
					state_text = "REACQ";
					break;
				default:
					break;
				}
				cv::putText(annotated, state_text, cv::Point(5, 20),
				            cv::FONT_HERSHEY_SIMPLEX, 0.7,
				            cv::Scalar(0, 165, 255), 2);
				if(!result.error_code.empty())
					cv::putText(annotated, result.error_code,
					            cv::Point(5, 45),
					            cv::FONT_HERSHEY_SIMPLEX, 0.5,
					            cv::Scalar(0, 0, 255), 1);
			}

			cv::resize(annotated, top_left,
			           cv::Size(top_left.cols, top_left.rows), 0.0, 0.0,
			           cv::INTER_AREA);

			// ── 右上：展开管道（原始彩色）──
			cv::Mat top_right = canvas(cv::Rect(kW, 0, disp_w, disp_h));
			if(!debug_warped_pipe_.empty())
			{
				cv::resize(debug_warped_pipe_, top_right,
				           cv::Size(top_right.cols, top_right.rows),
				           0.0, 0.0, cv::INTER_AREA);
			}

			// ── 左下：棕色管道掩膜 ──
			cv::Mat bottom_left =
			    canvas(cv::Rect(0, kH / 2, kW, kH / 2));
			if(!debug_track_mask_.empty())
			{
				cv::Mat mask_color;
				cv::cvtColor(debug_track_mask_, mask_color,
				             cv::COLOR_GRAY2BGR);
				cv::resize(mask_color, bottom_left,
				           cv::Size(bottom_left.cols, bottom_left.rows),
				           0.0, 0.0, cv::INTER_NEAREST);
			}

			// ── 右下：钢球二值图 + 状态 ──
			cv::Mat bottom_right =
			    canvas(cv::Rect(kW, kH / 2, disp_w, kH / 2));
			if(!debug_ball_binary_.empty())
			{
				cv::Mat bin_color;
				cv::cvtColor(debug_ball_binary_, bin_color,
				             cv::COLOR_GRAY2BGR);
				cv::resize(
				    bin_color, bottom_right,
				    cv::Size(bottom_right.cols, bottom_right.rows), 0.0,
				    0.0, cv::INTER_NEAREST);
			}
			// 叠加状态文本
			std::string status_text;
			if(result.valid && result.calibrated)
			{
				status_text = "OK offset="
				    + std::to_string(result.offset_mm) + "mm" + " conf="
				    + std::to_string(static_cast<int>(
				        std::lround(result.confidence * 100.0)))
				    + "%";
			}
			else if(result.error_code == "ZERO_CALIBRATING")
			{
				status_text = "CALIBRATING "
				    + std::to_string(zero_buffer_.size()) + "/"
				    + std::to_string(ball.zero_samples);
			}
			else
			{
				status_text = result.error_code;
			}
			cv::putText(bottom_right, status_text, cv::Point(5, 20),
			            cv::FONT_HERSHEY_SIMPLEX, 0.7,
			            cv::Scalar(200, 200, 200), 1);

			cv::resize(canvas, frame, cv::Size(frame.cols, frame.rows),
			           0.0, 0.0, cv::INTER_LINEAR);
		}
		catch(const cv::Exception& error)
		{
			ETEST_LOG_ERROR(
			    "VISION",
			    std::string("ball draw exception: ") + error.what());
		}
		catch(...)
		{
			ETEST_LOG_ERROR("VISION", "unknown ball draw exception");
		}
	}

	// ────────────────────────────────────────────────────────────
	// 神经网络检测
	// ────────────────────────────────────────────────────────────

	bool VisionProcessor::loadNnModel(
	    const std::string& onnx_path,
	    const std::string& class_names_path,
	    double confidence_threshold, double nms_threshold) noexcept
	{
		try
		{
			nn_net_ = cv::dnn::readNetFromONNX(onnx_path);
			if(nn_net_.empty())
			{
				ETEST_LOG_ERROR(
				    "VISION_NN",
				    "failed to load ONNX model: " + onnx_path);
				nn_loaded_ = false;
				return false;
			}

			nn_confidence_threshold_ = confidence_threshold;
			nn_nms_threshold_ = nms_threshold;
			nn_class_names_.clear();

			if(!class_names_path.empty())
			{
				std::ifstream class_file(class_names_path);
				if(class_file.is_open())
				{
					std::string name;
					while(std::getline(class_file, name))
					{
						if(!name.empty())
							nn_class_names_.push_back(name);
					}
				}
			}

			nn_output_names_ = nn_net_.getUnconnectedOutLayersNames();
			nn_loaded_ = true;
			return true;
		}
		catch(const cv::Exception& error)
		{
			ETEST_LOG_ERROR(
			    "VISION_NN",
			    std::string("failed to load ONNX: ") + error.what());
		}
		catch(const std::exception& error)
		{
			ETEST_LOG_ERROR(
			    "VISION_NN",
			    std::string("failed to load ONNX: ") + error.what());
		}
		catch(...)
		{
			ETEST_LOG_ERROR("VISION_NN", "unknown ONNX load exception");
		}

		nn_loaded_ = false;
		return false;
	}

	cv::Mat VisionProcessor::detectNn(const cv::Mat& frame) noexcept
	{
		try
		{
			if(!nn_loaded_ || frame.empty())
				return frame.clone();

			constexpr int in_w = 640;
			constexpr int in_h = 640;

			cv::Mat blob = cv::dnn::blobFromImage(
			    frame, 1.0 / 255.0, cv::Size(in_w, in_h), cv::Scalar(),
			    true, false);

			nn_net_.setInput(blob);

			std::vector<cv::Mat> outputs;
			nn_net_.forward(outputs, nn_output_names_);

			const float fw = static_cast<float>(frame.cols);
			const float fh = static_cast<float>(frame.rows);
			const float xs = fw / in_w;
			const float ys = fh / in_h;

			std::vector<cv::Rect> boxes;
			std::vector<float> confidences;
			std::vector<int> class_ids;

			for(const auto& output: outputs)
			{
				const auto* data =
				    reinterpret_cast<const float*>(output.data);
				const int rows = output.size[1];
				const int cols = output.size[2];

				for(int r = 0; r < rows; ++r)
				{
					const float* rd = data + r * cols;
					const float obj = rd[4];
					if(obj < nn_confidence_threshold_)
						continue;

					float max_cls = 0.0F;
					int best_cls = 0;
					for(int c = 0; c < 80; ++c)
					{
						const float cf = rd[5 + c];
						if(cf > max_cls)
						{
							max_cls = cf;
							best_cls = c;
						}
					}

					const float fc = obj * max_cls;
					if(fc < nn_confidence_threshold_)
						continue;

					const float cx = rd[0];
					const float cy = rd[1];
					const float w2 = rd[2];
					const float h2 = rd[3];

					boxes.emplace_back(
					    static_cast<int>((cx - 0.5F * w2) * xs),
					    static_cast<int>((cy - 0.5F * h2) * ys),
					    static_cast<int>(w2 * xs),
					    static_cast<int>(h2 * ys));
					confidences.push_back(fc);
					class_ids.push_back(best_cls);
				}
			}

			std::vector<int> nms_idx;
			cv::dnn::NMSBoxes(boxes, confidences,
			                  nn_confidence_threshold_,
			                  nn_nms_threshold_, nms_idx);

			last_detections_.clear();
			cv::Mat result = frame.clone();

			for(int idx: nms_idx)
			{
				const cv::Rect& box = boxes[idx];
				const int cid = class_ids[idx];
				const float conf = confidences[idx];

				std::string name = "class_" + std::to_string(cid);
				if(cid >= 0
				   && static_cast<std::size_t>(cid)
				       < nn_class_names_.size())
				{
					name = nn_class_names_[cid];
				}

				last_detections_.push_back(
				    {name, conf, box.x, box.y, box.x,
				     box.y + box.height, box.x + box.width,
				     box.y + box.height, box.x + box.width, box.y});

				cv::rectangle(result, box, cv::Scalar(0, 255, 0), 2);
				cv::putText(
				    result,
				    name + " "
				        + std::to_string(static_cast<int>(conf * 100))
				        + "%",
				    cv::Point(box.x, box.y - 5),
				    cv::FONT_HERSHEY_SIMPLEX, 0.5,
				    cv::Scalar(0, 255, 0), 1);
			}

			return result;
		}
		catch(const cv::Exception& error)
		{
			ETEST_LOG_ERROR("VISION_NN",
			                std::string("detectNn: ") + error.what());
			return frame.clone();
		}
		catch(const std::exception& error)
		{
			ETEST_LOG_ERROR("VISION_NN",
			                std::string("detectNn: ") + error.what());
			return frame.clone();
		}
		catch(...)
		{
			ETEST_LOG_ERROR("VISION_NN", "unknown detectNn exception");
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