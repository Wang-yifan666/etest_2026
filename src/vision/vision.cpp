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

	// 在模式分发前预标记 target_type，确保异常/空帧路径也正确
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
	cv::Mat mask;

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

	mask = mask1 | mask2;

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
	result.confidence = 1.0; // 颜色检测置信度为 1

	const cv::RotatedRect rectangle = cv::minAreaRect(*largest);

	result.angle = rectangle.angle;

	// 简单距离估算：基于面积
	result.distance = std::sqrt(area);

	return result;
}

// ────────────────────────────────────────────────────────────
// Ball 检测
// ────────────────────────────────────────────────────────────

VisionResult VisionProcessor::detectBall(const cv::Mat& frame)
{
	VisionResult result;
	result.target_type = "BALL";

	const auto& ball = config_.ball;

	// 运行时校验 ROI 与轴线参数
	const int fw = frame.cols;
	const int fh = frame.rows;
	const cv::Rect roi(ball.roi_x, ball.roi_y, ball.roi_w, ball.roi_h);
	const cv::Rect frame_rect(0, 0, fw, fh);

	if((roi & frame_rect) != roi)
	{
		if(!ball_config_error_reported_)
		{
			ETEST_LOG_ERROR("VISION",
			                "Ball ROI is outside frame: "
			                    + std::to_string(fw) + "x"
			                    + std::to_string(fh));
			ball_config_error_reported_ = true;
		}
		result.error_code = "INVALID_BALL_CONFIG";
		return result;
	}

	if(ball.bg_kernel < 3 || ball.bg_kernel % 2 == 0
	   || ball.morph_kernel < 1 || ball.morph_kernel % 2 == 0)
	{
		if(!ball_config_error_reported_)
		{
			ETEST_LOG_ERROR("VISION",
			                "Ball kernel config invalid");
			ball_config_error_reported_ = true;
		}
		result.error_code = "INVALID_BALL_CONFIG";
		return result;
	}

	const cv::Point2f axis_p1(
	    static_cast<float>(ball.axis_x1),
	    static_cast<float>(ball.axis_y1));
	const cv::Point2f axis_p2(
	    static_cast<float>(ball.axis_x2),
	    static_cast<float>(ball.axis_y2));
	const cv::Point2f axis_vec = axis_p2 - axis_p1;
	const double axis_len = cv::norm(axis_vec);

	if(axis_len < 1.0)
	{
		if(!ball_config_error_reported_)
		{
			ETEST_LOG_ERROR("VISION",
			                "Ball axis too short");
			ball_config_error_reported_ = true;
		}
		result.error_code = "INVALID_BALL_CONFIG";
		return result;
	}

	const cv::Point2f axis_unit = axis_vec
	    / static_cast<float>(axis_len);

	ball_config_error_reported_ = false;

	// 1) ROI 裁剪 → 灰度 → 降噪
	const cv::Mat roi_image = frame(roi);

	cv::Mat gray;
	if(roi_image.channels() == 3)
	{
		cv::cvtColor(roi_image, gray, cv::COLOR_BGR2GRAY);
	}
	else
	{
		gray = roi_image.clone();
	}

	cv::GaussianBlur(gray, gray, cv::Size(5, 5), 0.0);

	// 2) 局部背景差分
	cv::Mat background;
	cv::GaussianBlur(
	    gray, background,
	    cv::Size(ball.bg_kernel, ball.bg_kernel), 0.0);

	cv::Mat diff;
	cv::absdiff(gray, background, diff);

	cv::Mat binary;
	cv::threshold(diff, binary, ball.threshold, 255,
	              cv::THRESH_BINARY);

	// 3) 形态学：CLOSE → OPEN
	const cv::Mat morph_kernel = cv::getStructuringElement(
	    cv::MORPH_ELLIPSE,
	    cv::Size(ball.morph_kernel, ball.morph_kernel));

	cv::morphologyEx(binary, binary, cv::MORPH_CLOSE, morph_kernel);
	cv::morphologyEx(binary, binary, cv::MORPH_OPEN, morph_kernel);

	// 4) 轮廓筛选
	std::vector<std::vector<cv::Point>> contours;
	cv::findContours(binary, contours, cv::RETR_EXTERNAL,
	                 cv::CHAIN_APPROX_SIMPLE);

	double best_score = -1e9;
	double best_circularity = 0.0;
	cv::Point2f best_center;

	const bool in_reacquire = ball_lost_frame_count_
	    >= ball.reacquire_after_lost_frames;

	for(const auto& contour: contours)
	{
		const double area = cv::contourArea(contour);
		if(area < ball.min_area || area > ball.max_area) continue;

		const double perimeter = cv::arcLength(contour, true);
		if(perimeter < 1.0) continue;

		const double circularity =
		    4.0 * CV_PI * area / (perimeter * perimeter);
		if(circularity < ball.min_circularity) continue;

		const cv::Rect box = cv::boundingRect(contour);
		const double aspect =
		    static_cast<double>(box.width)
		    / std::max(1, box.height);
		if(aspect < 0.45 || aspect > 2.20) continue;

		const cv::Moments moments = cv::moments(contour);
		if(std::abs(moments.m00) < 1e-6) continue;

		const cv::Point2f center(
		    static_cast<float>(moments.m10 / moments.m00
		                       + ball.roi_x),
		    static_cast<float>(moments.m01 / moments.m00
		                       + ball.roi_y));

		// 到轴线垂直距离
		const cv::Point2f from_axis = center - axis_p1;
		const double projected =
		    from_axis.x * axis_unit.x
		    + from_axis.y * axis_unit.y;
		const cv::Point2f nearest =
		    axis_p1 + axis_unit * static_cast<float>(projected);
		const double axis_distance =
		    cv::norm(center - nearest);

		if(axis_distance > ball.max_axis_distance_px) continue;

		// 帧间跳变检查（重捕获模式下跳过）
		double jump_distance = 0.0;
		if(!in_reacquire && has_last_ball_center_)
		{
			jump_distance =
			    cv::norm(center - last_ball_center_);
			if(jump_distance > ball.max_jump_px) continue;
		}

		const double score = circularity * 100.0
		    - axis_distance * 1.5
		    - jump_distance * 0.2;

		if(score > best_score)
		{
			best_score = score;
			best_center = center;
			best_circularity = circularity;
		}
	}

	// 5) 无候选 → 丢球
	if(best_score < -1e7)
	{
		++ball_lost_frame_count_;

		if(!ball_lost_)
		{
			ball_lost_ = true;
			ETEST_LOG_WARN("VISION",
			               "Ball lost");
			last_ball_lost_log_time_ =
			    std::chrono::steady_clock::now();
		}
		else
		{
			// 每 1000ms 节流日志
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

		// 重捕获：超过阈值帧后放弃旧参考点
		if(ball_lost_frame_count_
		   >= ball.reacquire_after_lost_frames)
		{
			has_last_ball_center_ = false;
			ball_filter_initialized_ = false;
		}

		// 未锁定零标定时丢球 → 清空缓冲区
		if(!zero_locked_)
		{
			zero_buffer_.clear();
		}

		result.x = best_center.x;
		result.y = best_center.y;
		result.error_code = "BALL_LOST";
		return result;
	}

	// 6) 检测成功
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

	// 7) 轴线投影
	const cv::Point2f from_start = best_center - axis_p1;
	const double axis_position_px =
	    from_start.x * axis_unit.x
	    + from_start.y * axis_unit.y;

	// 8) 零点校准
	if(!zero_locked_)
	{
		if(ball.zero_mode == "fixed")
		{
			zero_position_px_ = ball.zero_position_px;
			zero_locked_ = true;
			ETEST_LOG_INFO(
			    "VISION",
			    "Ball zero locked (fixed): "
			    + std::to_string(zero_position_px_)
			    + " px");
		}
		else // startup
		{
			// 跳变过大 → 清空缓冲区
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

			zero_buffer_.push_back(
			    axis_position_px);

			while(static_cast<int>(
			          zero_buffer_.size())
			      > ball.zero_samples)
			{
				zero_buffer_.pop_front();
			}

			if(static_cast<int>(zero_buffer_.size())
			   >= ball.zero_samples)
			{
				double sum = 0.0;
				for(double v: zero_buffer_)
				{
					sum += v;
				}
				const double mean =
				    sum
				    / static_cast<double>(
				        zero_buffer_.size());

				double var = 0.0;
				for(double v: zero_buffer_)
				{
					const double d = v - mean;
					var += d * d;
				}
				var /= static_cast<double>(
				    zero_buffer_.size());
				const double stddev =
				    std::sqrt(var);

				if(stddev <= ball.zero_std_px)
				{
					zero_position_px_ = mean;
					zero_locked_ = true;
					ETEST_LOG_INFO(
					    "VISION",
					    "Ball zero calibrated: "
					    + std::to_string(mean)
					    + " px, std="
					    + std::to_string(
					        stddev));
				}
			}
		}
	}

	if(!zero_locked_)
	{
		result.error_code = "ZERO_CALIBRATING";
		return result;
	}

	// 9) 厘米换算 + 低通滤波
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

	// 10) 填充结果
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
// 调试绘制
// ────────────────────────────────────────────────────────────

void VisionProcessor::drawDebugInfo(
    cv::Mat& frame, const VisionResult& result) noexcept
{
	try
	{
		if(frame.empty())
		{
			ETEST_LOG_WARN("VISION",
			               "drawDebugInfo received an empty frame");
			return;
		}

		// Ball 模式使用独立绘制
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

		// 绿色 ROI 矩形
		cv::rectangle(
		    frame,
		    cv::Rect(ball.roi_x, ball.roi_y, ball.roi_w,
		             ball.roi_h),
		    cv::Scalar(0, 255, 0), 1);

		// 黄色轴线
		cv::line(frame,
		         cv::Point(static_cast<int>(ball.axis_x1),
		                   static_cast<int>(ball.axis_y1)),
		         cv::Point(static_cast<int>(ball.axis_x2),
		                   static_cast<int>(ball.axis_y2)),
		         cv::Scalar(0, 255, 255), 1);

		// 蓝色零点标线
		if(zero_locked_)
		{
			const cv::Point2f axis_p1(
			    static_cast<float>(ball.axis_x1),
			    static_cast<float>(ball.axis_y1));
			const cv::Point2f axis_p2(
			    static_cast<float>(ball.axis_x2),
			    static_cast<float>(ball.axis_y2));
			const cv::Point2f axis_vec = axis_p2 - axis_p1;
			const double axis_len = cv::norm(axis_vec);
			if(axis_len >= 1.0)
			{
				const cv::Point2f axis_unit =
				    axis_vec
				    / static_cast<float>(axis_len);
				const cv::Point2f zero_pt =
				    axis_p1
				    + axis_unit
				        * static_cast<float>(
				            zero_position_px_);
				cv::drawMarker(
				    frame,
				    cv::Point(
				        static_cast<int>(zero_pt.x),
				        static_cast<int>(zero_pt.y)),
				    cv::Scalar(255, 0, 0),
				    cv::MARKER_TILTED_CROSS, 10, 1);
			}
		}

		if(result.valid && result.calibrated)
		{
			// 红点标注球心
			cv::circle(
			    frame,
			    cv::Point(static_cast<int>(result.x),
			              static_cast<int>(result.y)),
			    6, cv::Scalar(0, 0, 255), -1);

			const int conf_pct =
			    static_cast<int>(std::lround(
			        result.confidence * 100.0));

			std::string label = "BALL "
			    + std::to_string(result.offset_mm)
			    + "mm OK " + std::to_string(conf_pct)
			    + "%";

			cv::putText(frame, label, cv::Point(10, 30),
			            cv::FONT_HERSHEY_SIMPLEX, 0.6,
			            cv::Scalar(0, 255, 0), 2);
		}
		else if(result.error_code == "BALL_LOST")
		{
			cv::putText(
			    frame,
			    "BALL LOST (" + std::to_string(
			        ball_lost_frame_count_)
			        + " frames)",
			    cv::Point(10, 30),
			    cv::FONT_HERSHEY_SIMPLEX, 0.6,
			    cv::Scalar(0, 0, 255), 2);
		}
		else if(result.error_code == "ZERO_CALIBRATING")
		{
			// 标定中但仍标注球心
			cv::circle(
			    frame,
			    cv::Point(static_cast<int>(result.x),
			              static_cast<int>(result.y)),
			    6, cv::Scalar(0, 165, 255), -1);

			cv::putText(
			    frame,
			    "CALIBRATING ("
			        + std::to_string(
			            zero_buffer_.size())
			        + "/"
			        + std::to_string(ball.zero_samples)
			        + ")",
			    cv::Point(10, 30),
			    cv::FONT_HERSHEY_SIMPLEX, 0.6,
			    cv::Scalar(0, 165, 255), 2);
		}
		else
		{
			cv::putText(
			    frame,
			    "BALL ERROR: " + result.error_code,
			    cv::Point(10, 30),
			    cv::FONT_HERSHEY_SIMPLEX, 0.6,
			    cv::Scalar(0, 0, 255), 2);
		}
	}
	catch(const cv::Exception& error)
	{
		ETEST_LOG_ERROR(
		    "VISION",
		    std::string("ball draw exception: ") + error.what());
	}
	catch(...)
	{
		ETEST_LOG_ERROR("VISION",
		                "unknown ball draw exception");
	}
}

// ────────────────────────────────────────────────────────────
// 神经网络检测（保持不变）
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
					{
						nn_class_names_.push_back(name);
					}
				}

				ETEST_LOG_INFO(
				    "VISION_NN",
				    "loaded "
				        + std::to_string(
				            nn_class_names_.size())
				        + " class names from "
				        + class_names_path);
			}
			else
			{
				ETEST_LOG_WARN(
				    "VISION_NN",
				    "class names file not found: "
				        + class_names_path
				        + "; detection boxes will show class ids");
			}
		}

		nn_output_names_ =
		    nn_net_.getUnconnectedOutLayersNames();

		ETEST_LOG_INFO(
		    "VISION_NN",
		    "ONNX model loaded successfully: " + onnx_path
		        + ", outputs="
		        + std::to_string(
		            nn_output_names_.size()));

		nn_loaded_ = true;
		return true;
	}
	catch(const cv::Exception& error)
	{
		ETEST_LOG_ERROR("VISION_NN",
		                std::string("failed to load ONNX model: ")
		                    + error.what());

		nn_loaded_ = false;
		return false;
	}
	catch(const std::exception& error)
	{
		ETEST_LOG_ERROR("VISION_NN",
		                std::string("failed to load ONNX model: ")
		                    + error.what());

		nn_loaded_ = false;
		return false;
	}
	catch(...)
	{
		ETEST_LOG_ERROR(
		    "VISION_NN",
		    "unknown exception while loading ONNX model");

		nn_loaded_ = false;
		return false;
	}
}

cv::Mat VisionProcessor::detectNn(const cv::Mat& frame) noexcept
{
	try
	{
		if(!nn_loaded_ || frame.empty())
		{
			return frame.clone();
		}

		constexpr int input_width = 640;
		constexpr int input_height = 640;

		cv::Mat blob = cv::dnn::blobFromImage(
		    frame, 1.0 / 255.0,
		    cv::Size(input_width, input_height),
		    cv::Scalar(), true, false);

		nn_net_.setInput(blob);

		std::vector<cv::Mat> outputs;
		nn_net_.forward(outputs, nn_output_names_);

		const float frame_width =
		    static_cast<float>(frame.cols);

		const float frame_height =
		    static_cast<float>(frame.rows);

		const float x_scale = frame_width / input_width;
		const float y_scale = frame_height / input_height;

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
				const float* row_data = data + r * cols;

				const float obj_conf = row_data[4];

				if(obj_conf < nn_confidence_threshold_)
				{
					continue;
				}

				float max_class_conf = 0.0F;
				int best_class_id = 0;

				for(int c = 0; c < 80; ++c)
				{
					const float class_conf =
					    row_data[5 + c];

					if(class_conf > max_class_conf)
					{
						max_class_conf = class_conf;
						best_class_id = c;
					}
				}

				const float final_conf =
				    obj_conf * max_class_conf;

				if(final_conf < nn_confidence_threshold_)
				{
					continue;
				}

				const float cx = row_data[0];
				const float cy = row_data[1];
				const float w = row_data[2];
				const float h = row_data[3];

				const int x =
				    static_cast<int>(
				        (cx - 0.5F * w) * x_scale);

				const int y =
				    static_cast<int>(
				        (cy - 0.5F * h) * y_scale);

				const int width =
				    static_cast<int>(w * x_scale);
				const int height =
				    static_cast<int>(h * y_scale);

				boxes.emplace_back(x, y, width, height);
				confidences.push_back(final_conf);
				class_ids.push_back(best_class_id);
			}
		}

		std::vector<int> nms_indices;
		cv::dnn::NMSBoxes(boxes, confidences,
		                  nn_confidence_threshold_,
		                  nn_nms_threshold_, nms_indices);

		last_detections_.clear();

		cv::Mat result = frame.clone();

		for(int idx: nms_indices)
		{
			const cv::Rect& box = boxes[idx];
			const int class_id = class_ids[idx];
			const float conf = confidences[idx];

			std::string class_name;

			if(class_id >= 0
			   && static_cast<std::size_t>(class_id)
			       < nn_class_names_.size())
			{
				class_name =
				    nn_class_names_[class_id];
			}
			else
			{
				class_name =
				    "class_"
				    + std::to_string(class_id);
			}

			last_detections_.push_back(
			    {class_name, conf, box.x, box.y, box.x,
			     box.y + box.height,
			     box.x + box.width,
			     box.y + box.height,
			     box.x + box.width, box.y});

			const cv::Scalar color(
			    (class_id * 37 + 80) % 255,
			    (class_id * 73 + 160) % 255,
			    (class_id * 113 + 40) % 255);

			cv::rectangle(result, box, color, 2);

			std::string label = class_name;

			label +=
			    " "
			    + std::to_string(
			        static_cast<int>(conf * 100))
			    + "%";

			int baseline = 0;
			const cv::Size text_size = cv::getTextSize(
			    label, cv::FONT_HERSHEY_SIMPLEX, 0.5, 2,
			    &baseline);

			cv::rectangle(
			    result,
			    cv::Point(box.x,
			              box.y - text_size.height - 5),
			    cv::Point(box.x + text_size.width,
			              box.y),
			    color, cv::FILLED);

			cv::putText(result, label,
			            cv::Point(box.x, box.y - 5),
			            cv::FONT_HERSHEY_SIMPLEX, 0.5,
			            cv::Scalar(255, 255, 255), 2);
		}

		return result;
	}
	catch(const cv::Exception& error)
	{
		ETEST_LOG_ERROR(
		    "VISION_NN",
		    std::string("detectNn exception: ")
		        + error.what());

		return frame.clone();
	}
	catch(const std::exception& error)
	{
		ETEST_LOG_ERROR(
		    "VISION_NN",
		    std::string("detectNn exception: ")
		        + error.what());

		return frame.clone();
	}
	catch(...)
	{
		ETEST_LOG_ERROR("VISION_NN",
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