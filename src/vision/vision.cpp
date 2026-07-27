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

		// 加载类别名文件（可选）
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
				        + std::to_string(nn_class_names_.size())
				        + " class names from " + class_names_path);
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

		// 获取输出层名称
		nn_output_names_ = nn_net_.getUnconnectedOutLayersNames();

		ETEST_LOG_INFO(
		    "VISION_NN",
		    "ONNX model loaded successfully: " + onnx_path
		        + ", outputs="
		        + std::to_string(nn_output_names_.size()));

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

		// YOLOv5 输入尺寸。
		constexpr int input_width = 640;
		constexpr int input_height = 640;

		// 构建 blob。
		cv::Mat blob = cv::dnn::blobFromImage(
		    frame, 1.0 / 255.0, cv::Size(input_width, input_height),
		    cv::Scalar(), true, false);

		nn_net_.setInput(blob);

		std::vector<cv::Mat> outputs;
		nn_net_.forward(outputs, nn_output_names_);

		// YOLOv5 输出形状：[1, num_detections, 85]
		// 85 = cx, cy, w, h, obj_conf, class_0, ..., class_79
		const float frame_width = static_cast<float>(frame.cols);

		const float frame_height = static_cast<float>(frame.rows);

		const float x_scale = frame_width / input_width;
		const float y_scale = frame_height / input_height;

		std::vector<cv::Rect> boxes;
		std::vector<float> confidences;
		std::vector<int> class_ids;

		for(const auto& output : outputs)
		{
			const auto* data =
			    reinterpret_cast<const float*>(output.data);

			const int rows = output.size[1]; // num_detections
			const int cols = output.size[2]; // 85

			for(int r = 0; r < rows; ++r)
			{
				const float* row_data = data + r * cols;

				const float obj_conf = row_data[4];

				if(obj_conf < nn_confidence_threshold_)
				{
					continue;
				}

				// 找最大类别置信度。
				float max_class_conf = 0.0F;
				int best_class_id = 0;

				for(int c = 0; c < 80; ++c)
				{
					const float class_conf = row_data[5 + c];

					if(class_conf > max_class_conf)
					{
						max_class_conf = class_conf;
						best_class_id = c;
					}
				}

				const float final_conf = obj_conf * max_class_conf;

				if(final_conf < nn_confidence_threshold_)
				{
					continue;
				}

				// 解析坐标（YOLOv5: cx, cy, w, h，归一化到 [0,1]）。
				const float cx = row_data[0];
				const float cy = row_data[1];
				const float w = row_data[2];
				const float h = row_data[3];

				const int x =
				    static_cast<int>((cx - 0.5F * w) * x_scale);

				const int y =
				    static_cast<int>((cy - 0.5F * h) * y_scale);

				const int width = static_cast<int>(w * x_scale);
				const int height = static_cast<int>(h * y_scale);

				boxes.emplace_back(x, y, width, height);
				confidences.push_back(final_conf);
				class_ids.push_back(best_class_id);
			}
		}

		// NMS
		std::vector<int> nms_indices;
		cv::dnn::NMSBoxes(boxes, confidences,
		                  nn_confidence_threshold_,
		                  nn_nms_threshold_, nms_indices);

		// 填充检测结果，供日志输出。
		last_detections_.clear();

		// 绘制结果。
		cv::Mat result = frame.clone();

		for(int idx : nms_indices)
		{
			const cv::Rect& box = boxes[idx];
			const int class_id = class_ids[idx];
			const float conf = confidences[idx];

			std::string class_name;

			if(class_id >= 0
			   && static_cast<std::size_t>(class_id)
			       < nn_class_names_.size())
			{
				class_name = nn_class_names_[class_id];
			}
			else
			{
				class_name = "class_" + std::to_string(class_id);
			}

			last_detections_.push_back(
			    {class_name, conf, box.x, box.y, box.x,
			     box.y + box.height, box.x + box.width,
			     box.y + box.height, box.x + box.width, box.y});

			// 随机颜色。
			const cv::Scalar color((class_id * 37 + 80) % 255,
			                       (class_id * 73 + 160) % 255,
			                       (class_id * 113 + 40) % 255);

			cv::rectangle(result, box, color, 2);

			std::string label = class_name;

			label += " "
			    + std::to_string(static_cast<int>(conf * 100))
			    + "%";

			int baseline = 0;
			const cv::Size text_size = cv::getTextSize(
			    label, cv::FONT_HERSHEY_SIMPLEX, 0.5, 2, &baseline);

			cv::rectangle(
			    result,
			    cv::Point(box.x, box.y - text_size.height - 5),
			    cv::Point(box.x + text_size.width, box.y), color,
			    cv::FILLED);

			cv::putText(result, label, cv::Point(box.x, box.y - 5),
			            cv::FONT_HERSHEY_SIMPLEX, 0.5,
			            cv::Scalar(255, 255, 255), 2);
		}

		return result;
	}
	catch(const cv::Exception& error)
	{
		ETEST_LOG_ERROR(
		    "VISION_NN",
		    std::string("detectNn exception: ") + error.what());

		return frame.clone();
	}
	catch(const std::exception& error)
	{
		ETEST_LOG_ERROR(
		    "VISION_NN",
		    std::string("detectNn exception: ") + error.what());

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