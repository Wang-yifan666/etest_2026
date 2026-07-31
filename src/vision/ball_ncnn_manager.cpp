#include "vision/ball_ncnn_manager.hpp"

#include "core/logger.hpp"
#include "vision/roi_utils.hpp"
#include "vision/yolo_backend.hpp"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <string>

namespace etest::vision
{

	bool BallNcnnManager::initialize(const BallNcnnConfig& config,
	                                 std::string& error) noexcept
	{
		config_ = config;

		if(!config_.enabled)
		{
			error = "ball_ncnn is disabled in config";
			return false;
		}

		// ── 加载完整模型（640×160）──
		if(!config_.full_model_param.empty())
		{
			full_detector_ = createDetector(
			    config_.full_model_param, config_.full_input_width,
			    config_.full_input_height, config_.num_threads,
			    config_.use_fp16_storage, config_.use_fp16_arithmetic,
			    error);
			if(!full_detector_)
			{
				return false;
			}
			full_ready_ = true;
			ETEST_LOG_INFO(
			    "BALL_NCNN",
			    "full model loaded: " + config_.full_model_param
			        + " input="
			        + std::to_string(config_.full_input_width) + "x"
			        + std::to_string(config_.full_input_height));
		}
		else
		{
			error = "full_model_param is empty";
			return false;
		}

		// ── 加载中心模型（224×160）──
		if(!config_.center_model_param.empty())
		{
			center_detector_ = createDetector(
			    config_.center_model_param, config_.center_input_width,
			    config_.center_input_height, config_.num_threads,
			    config_.use_fp16_storage, config_.use_fp16_arithmetic,
			    error);
			if(!center_detector_)
			{
				return false;
			}
			center_ready_ = true;
			ETEST_LOG_INFO(
			    "BALL_NCNN",
			    "center model loaded: " + config_.center_model_param
			        + " input="
			        + std::to_string(config_.center_input_width) + "x"
			        + std::to_string(config_.center_input_height));
		}
		else
		{
			error = "center_model_param is empty";
			return false;
		}

		// ── 预热：各跑 3 帧 ──
		cv::Mat dummy = cv::Mat::zeros(config_.full_src_height,
		                               config_.full_src_width, CV_8UC3);

		for(int i = 0; i < 3; ++i)
		{
			YoloTiming dummy_timing;
			full_detector_->infer(dummy, &dummy_timing);
		}

		cv::Mat dummy_center =
		    cv::Mat::zeros(config_.center_src_height,
		                   config_.center_src_width, CV_8UC3);
		for(int i = 0; i < 3; ++i)
		{
			YoloTiming dummy_timing;
			center_detector_->infer(dummy_center, &dummy_timing);
		}

		ETEST_LOG_INFO("BALL_NCNN",
		               "dual models initialized and warmed up");

		return true;
	}

	BallMeasurement BallNcnnManager::process(
	    const cv::Mat& raw_frame, TrackingMode tracking_mode,
	    YoloTiming* timing) noexcept
	{
		BallMeasurement result;

		if(!full_ready_ || !center_ready_)
		{
			result.status = "ERROR";
			return result;
		}

		if(tracking_mode == TrackingMode::NONE)
		{
			result.status = "OK";
			result.valid = true;
			return result;
		}

		// ── 选择检测器和 ROI ──
		YoloDetector* detector = nullptr;
		roi_utils::InferenceRoi roi;

		if(tracking_mode == TrackingMode::CENTER)
		{
			if(!center_ready_)
			{
				result.status = "ERROR";
				return result;
			}
			detector = center_detector_.get();
			roi = roi_utils::getCenterInferenceRoi(raw_frame.size(),
			                                       config_);
		}
		else
		{
			// FULL 或 FULL_REACQUIRE
			detector = full_detector_.get();
			roi = roi_utils::getFullInferenceRoi(raw_frame.size(),
			                                     config_);
		}

		return detectWithRoi(raw_frame, roi, *detector, timing);
	}

	void BallNcnnManager::resetTracking() noexcept
	{
		tracking_initialized_ = false;
		last_global_center_ = {-1.0F, -1.0F};
	}

	bool BallNcnnManager::fullModelReady() const noexcept
	{
		return full_ready_;
	}

	bool BallNcnnManager::centerModelReady() const noexcept
	{
		return center_ready_;
	}

	// ── 私有实现 ──

	BallMeasurement BallNcnnManager::detectWithRoi(
	    const cv::Mat& raw_frame, const roi_utils::InferenceRoi& roi,
	    YoloDetector& detector, YoloTiming* timing) noexcept
	{
		BallMeasurement result;
		result.status = "LOST";

		// 裁剪 ROI：拒绝静默裁剪，确保 GUI 配置与运行时完全一致
		const cv::Rect frame_rect(0, 0, raw_frame.cols, raw_frame.rows);
		const cv::Rect clamped = roi.rect & frame_rect;

		if(clamped.empty())
		{
			result.status = "ERROR";
			return result;
		}

		if(clamped != roi.rect)
		{
			ETEST_LOG_ERROR("BALL_NCNN",
			                "Configured ROI exceeds frame boundary: "
			                "roi=("
			                    + std::to_string(roi.rect.x) + ","
			                    + std::to_string(roi.rect.y) + ","
			                    + std::to_string(roi.rect.width) + ","
			                    + std::to_string(roi.rect.height)
			                    + "), frame=("
			                    + std::to_string(raw_frame.cols) + ","
			                    + std::to_string(raw_frame.rows) + ")");

			result.status = "ERROR";
			return result;
		}

		cv::Mat roi_frame = raw_frame(roi.rect);

		// 推理
		YoloTiming local_timing;
		const auto detections =
		    detector.infer(roi_frame, &local_timing);

		if(timing != nullptr)
			*timing = local_timing;

		result.inference_ms = local_timing.total_ms;

		if(detections.empty())
		{
			if(tracking_initialized_)
				result.status = "LOST";
			return result;
		}

		// ── 选球 ──
		const YoloDetection* best = nullptr;
		if(!tracking_initialized_)
		{
			// 选置信度最高的
			float best_conf =
			    static_cast<float>(config_.minimum_confidence);
			for(const auto& d: detections)
			{
				if(d.confidence > best_conf)
				{
					best_conf = d.confidence;
					best = &d;
				}
			}
		}
		else
		{
			// 跟踪模式：选距离上一帧最近的
			double best_dist = 1e9;
			for(const auto& d: detections)
			{
				if(d.confidence < config_.minimum_confidence)
					continue;

				cv::Point2f local_center = d.center();
				cv::Point2f global_center =
				    roi_utils::localToGlobal(local_center, clamped);
				double dist =
				    cv::norm(global_center - last_global_center_);

				if(dist < best_dist)
				{
					best_dist = dist;
					best = &d;
				}
			}
		}

		if(best == nullptr)
		{
			if(tracking_initialized_)
				result.status = "LOST";
			return result;
		}

		// ── 坐标恢复 ──
		cv::Point2f local_center = best->center();
		cv::Point2f global_center =
		    roi_utils::localToGlobal(local_center, clamped);

		// 跳变拒绝
		if(tracking_initialized_)
		{
			constexpr double kMaxJumpPx = 80.0;
			double jump = cv::norm(global_center - last_global_center_);
			if(jump > kMaxJumpPx)
			{
				result.status = "LOST";
				return result;
			}
		}

		// ── 填充结果 ──
		result.valid = true;
		result.local_center = local_center;
		result.global_center = global_center;
		result.confidence = best->confidence;
		result.status = "OK";

		// 物理坐标转换（0.1mm 单位）
		result.position_0p1mm = roi_utils::pixelTo0p1mm(
		    global_center.x, config_.axis_calibration);

		last_global_center_ = global_center;
		tracking_initialized_ = true;

		return result;
	}

	std::unique_ptr<YoloDetector> BallNcnnManager::createDetector(
	    const std::string& param_path, int input_width,
	    int input_height, int num_threads, bool fp16_storage,
	    bool fp16_arithmetic, std::string& error) const noexcept
	{
		YoloBackendConfig backend_config;
		backend_config.input_width = input_width;
		backend_config.input_height = input_height;
		backend_config.num_threads = num_threads;
		backend_config.use_fp16_storage = fp16_storage;
		backend_config.use_fp16_arithmetic = fp16_arithmetic;
		backend_config.use_vulkan = false;
		backend_config.input_blob = "in0";
		backend_config.output_blob = "out0";

#ifdef ETEST_HAS_NCNN
		auto backend = createNcnnBackend(param_path, backend_config);
		if(!backend)
		{
			error = "failed to create NCNN backend: " + param_path;
			return nullptr;
		}

		auto detector = std::make_unique<YoloDetector>();
		std::vector<std::string> class_names = {"ball"};
		std::string init_error;

		if(!detector->initialize(
		       std::move(backend), backend_config,
		       std::move(class_names),
		       static_cast<float>(config_.minimum_confidence), 0.45F,
		       init_error))
		{
			error = "detector init failed: " + init_error;
			return nullptr;
		}

		return detector;
#else
		error = "NCNN support not compiled (ETEST_HAS_NCNN=OFF)";
		return nullptr;
#endif
	}

} // namespace etest::vision