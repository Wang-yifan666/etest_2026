#include "vision/yolo_detector.hpp"

#include "core/logger.hpp"

#include <opencv2/dnn.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace etest::vision
{

	namespace
	{
		using Clock = std::chrono::steady_clock;

		double toMs(const Clock::time_point& a,
		            const Clock::time_point& b)
		{
			return std::chrono::duration<double, std::milli>(b - a)
			    .count();
		}
	} // namespace

	bool YoloDetector::initialize(
	    std::unique_ptr<IYoloBackend> backend,
	    const YoloBackendConfig& backend_config,
	    std::vector<std::string> class_names,
	    float confidence_threshold, float nms_threshold,
	    std::string& error) noexcept
	{
		if(!backend)
		{
			error = "backend is null";
			return false;
		}

		backend_config_ = backend_config;
		class_names_ = std::move(class_names);
		confidence_threshold_ = confidence_threshold;
		nms_threshold_ = nms_threshold;
		first_inference_ = true;

		backend_ = std::move(backend);

		if(!backend_->ready())
		{
			error = "backend reports not ready after construction";
			return false;
		}

		return true;
	}

	std::vector<YoloDetection> YoloDetector::infer(
	    const cv::Mat& frame, YoloTiming* timing) noexcept
	{
		std::vector<YoloDetection> detections;

		try
		{
			if(!backend_ || !backend_->ready())
			{
				ETEST_LOG_ERROR("YOLO_DETECTOR", "backend not ready");
				return detections;
			}

			if(frame.empty())
			{
				ETEST_LOG_ERROR("YOLO_DETECTOR", "input frame empty");
				return detections;
			}

			YoloRawOutput raw;
			YoloTiming backend_timing;
			std::string error;

			const bool ok =
			    backend_->forward(frame, raw, &backend_timing, error);

			if(!ok)
			{
				ETEST_LOG_ERROR("YOLO_DETECTOR",
				                "forward failed: " + error);
				return detections;
			}

			if(!raw.valid())
			{
				ETEST_LOG_ERROR("YOLO_DETECTOR",
				                "invalid raw output from backend");
				return detections;
			}

			// 首次推理打印输出形状
			if(first_inference_)
			{
				first_inference_ = false;
				ETEST_LOG_INFO(
				    "YOLO_DETECTOR",
				    "raw output shape: rows=" + std::to_string(raw.rows)
				        + " columns=" + std::to_string(raw.columns));
			}

			// 解码 + NMS
			const cv::Size original_size(frame.cols, frame.rows);
			const auto t_decode_start = Clock::now();

			detections = decode(raw, original_size, nullptr);

			const auto t_decode_end = Clock::now();

			// 回填计时（后端负责 preprocess + forward，detector 负责 decode+nms）
			if(timing != nullptr)
			{
				timing->preprocess_ms = backend_timing.preprocess_ms;
				timing->forward_ms = backend_timing.forward_ms;
				timing->decode_ms = toMs(t_decode_start, t_decode_end);
				timing->nms_ms = 0.0; // NMS 包含在 decode 时间内
				timing->total_ms = timing->preprocess_ms
				    + timing->forward_ms + timing->decode_ms;
			}
		}
		catch(const cv::Exception& e)
		{
			ETEST_LOG_ERROR("YOLO_DETECTOR",
			                std::string("cv exception: ") + e.what());
		}
		catch(const std::exception& e)
		{
			ETEST_LOG_ERROR("YOLO_DETECTOR",
			                std::string("exception: ") + e.what());
		}
		catch(...)
		{
			ETEST_LOG_ERROR("YOLO_DETECTOR", "unknown exception");
		}

		return detections;
	}

	bool YoloDetector::ready() const noexcept
	{
		return backend_ != nullptr && backend_->ready();
	}

	const char* YoloDetector::backendName() const noexcept
	{
		if(!backend_)
			return "none";
		return backend_->name();
	}

	std::vector<YoloDetection> YoloDetector::decode(
	    const YoloRawOutput& raw, const cv::Size& original_size,
	    YoloTiming* timing) const
	{
		using Clock = std::chrono::steady_clock;

		std::vector<YoloDetection> detections;

		const auto t_start = Clock::now();

		const float scale_x = static_cast<float>(original_size.width)
		    / static_cast<float>(backend_config_.input_width);

		const float scale_y = static_cast<float>(original_size.height)
		    / static_cast<float>(backend_config_.input_height);

		const int row_count = raw.rows;
		const int column_count = raw.columns;
		const int class_count = column_count - 5;

		const float* data = raw.data.data();

		std::vector<cv::Rect> boxes;
		std::vector<float> confidences;
		std::vector<int> class_ids;

		boxes.reserve(row_count);
		confidences.reserve(row_count);
		class_ids.reserve(row_count);

		for(int row = 0; row < row_count; ++row)
		{
			const float* candidate = data + row * column_count;

			const float objectness = candidate[4];

			if(objectness < confidence_threshold_)
				continue;

			float best_class_score = 0.0F;
			int best_class_id = -1;

			for(int c = 0; c < class_count; ++c)
			{
				const float score = candidate[5 + c];
				if(score > best_class_score)
				{
					best_class_score = score;
					best_class_id = c;
				}
			}

			const float confidence = objectness * best_class_score;

			if(confidence < confidence_threshold_)
				continue;

			// xywh → cv::Rect（与当前 inferYolo 完全一致）
			const float center_x = candidate[0] * scale_x;
			const float center_y = candidate[1] * scale_y;
			const float width = candidate[2] * scale_x;
			const float height = candidate[3] * scale_y;

			cv::Rect box(static_cast<int>(center_x - width * 0.5F),
			             static_cast<int>(center_y - height * 0.5F),
			             static_cast<int>(width),
			             static_cast<int>(height));

			box &= cv::Rect(0, 0, original_size.width,
			                original_size.height);

			if(box.empty())
				continue;

			boxes.push_back(box);
			confidences.push_back(confidence);
			class_ids.push_back(best_class_id);
		}

		const auto t_after_loop = Clock::now();

		// NMS
		std::vector<int> kept_indices;
		cv::dnn::NMSBoxes(boxes, confidences,
		                  static_cast<float>(confidence_threshold_),
		                  static_cast<float>(nms_threshold_),
		                  kept_indices);

		for(const int idx: kept_indices)
		{
			detections.push_back(
			    {class_ids[idx], confidences[idx], boxes[idx]});
		}

		// 按置信度降序
		std::sort(detections.begin(), detections.end(),
		          [](const YoloDetection& a, const YoloDetection& b) {
			          return a.confidence > b.confidence;
		          });

		const auto t_end = Clock::now();

		if(timing != nullptr)
		{
			timing->decode_ms = toMs(t_start, t_after_loop);
			timing->nms_ms = toMs(t_after_loop, t_end);
			timing->total_ms = timing->decode_ms + timing->nms_ms;
		}

		return detections;
	}

} // namespace etest::vision