#include "vision/vision.hpp"
#include "vision/yolo_detector.hpp"

#include "core/config.hpp"
#include "core/logger.hpp"

#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <fstream>
#include <string>

namespace etest::vision
{

	// ── 公开接口 ──

	void VisionProcessor::setVisionEpoch(
	    std::uint64_t epoch_ns) noexcept
	{
		vision_epoch_ns_ = epoch_ns;
	}

	bool VisionProcessor::isYoloBallReady() const noexcept
	{
		return nn_loaded_ && !yolo_model_unhealthy_;
	}

	void VisionProcessor::resetYoloSession() noexcept
	{
		yolo_zero_samples_.clear();
		yolo_zero_locked_ = false;
		yolo_zero_origin_ = {0.0F, 0.0F};
		yolo_calib_frame_count_ = 0;
		yolo_lost_frames_ = 0;
		yolo_reacquire_confirm_ = 0;
		yolo_last_center_ = {-1.0F, -1.0F};
		yolo_tracking_initialized_ = false;
		yolo_filtered_x_ = 0.0;
		yolo_consecutive_errors_ = 0;
		yolo_last_detections_.clear();
		yolo_last_valid_confidence_ = 0.0;
		// 不重置 model_unhealthy_, shape_logged, epoch
	}

	bool VisionProcessor::tryReloadYoloModel(
	    const std::string& model_path,
	    const std::string& class_names_path) noexcept
	{
		const auto now = std::chrono::steady_clock::now();
		constexpr int min_reload_interval_ms = 2000;

		if(yolo_last_reload_attempt_.time_since_epoch().count() > 0)
		{
			const auto elapsed =
			    std::chrono::duration_cast<std::chrono::milliseconds>(
			        now - yolo_last_reload_attempt_);
			if(elapsed.count() < min_reload_interval_ms)
				return false;
		}

		yolo_last_reload_attempt_ = now;

		if(loadNnModel(model_path, class_names_path,
		               nn_confidence_threshold_, nn_nms_threshold_))
		{
			yolo_model_unhealthy_ = false;
			nn_net_.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
			nn_net_.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
			ETEST_LOG_INFO("VISION_YOLO",
			               "model reloaded successfully");
			return true;
		}

		ETEST_LOG_ERROR("VISION_YOLO", "model reload failed");
		return false;
	}

	// ── 推理引擎 ──

	std::vector<YoloDetection> VisionProcessor::inferYolo(
	    const cv::Mat& frame, YoloTiming* timing) noexcept
	{
		// 优先使用新架构 yolo_detector_
		if(yolo_detector_ && yolo_detector_->ready())
		{
			return yolo_detector_->infer(frame, timing);
		}

		// 回退到旧版 nn_net_（兼容 loadNnModel 加载的路径）
		using Clock = std::chrono::steady_clock;

		std::vector<YoloDetection> detections;

		YoloTiming local_timing;
		const auto t_start = Clock::now();

		try
		{
			if(frame.empty())
			{
				ETEST_LOG_ERROR("VISION_YOLO", "input frame is empty");
				return detections;
			}

			if(!nn_loaded_)
			{
				ETEST_LOG_ERROR("VISION_YOLO", "model is not loaded");
				return detections;
			}

			const int input_width = 640;
			const int input_height = 640;

			cv::Mat blob = cv::dnn::blobFromImage(
			    frame, 1.0 / 255.0, cv::Size(input_width, input_height),
			    cv::Scalar(), true, false);

			const auto t_after_preprocess = Clock::now();

			nn_net_.setInput(blob);

			std::vector<cv::Mat> outputs;
			nn_net_.forward(outputs, nn_output_names_);

			const auto t_after_forward = Clock::now();

			if(outputs.empty())
			{
				ETEST_LOG_ERROR("VISION_YOLO", "model output is empty");
				return detections;
			}

			cv::Mat output = outputs.front();

			if(!yolo_shape_logged_)
			{
				yolo_shape_logged_ = true;
				std::string shape_str = "[";
				for(int d = 0; d < output.dims; ++d)
				{
					if(d > 0)
						shape_str += ",";
					shape_str += std::to_string(output.size[d]);
				}
				shape_str += "]";
				ETEST_LOG_INFO("VISION_YOLO",
				               "output shape=" + shape_str);
			}

			if(output.dims != 3)
			{
				ETEST_LOG_ERROR("VISION_YOLO",
				                "unsupported output dimensions: "
				                    + std::to_string(output.dims));
				return detections;
			}

			const int row_count = output.size[1];
			const int column_count = output.size[2];

			if(column_count < 6)
			{
				ETEST_LOG_ERROR("VISION_YOLO",
				                "invalid output column count: "
				                    + std::to_string(column_count));
				return detections;
			}

			const int class_count = column_count - 5;

			if(class_count
			   != static_cast<int>(nn_class_names_.size()))
			{
				ETEST_LOG_ERROR(
				    "VISION_YOLO",
				    "class_count mismatch: model has "
				        + std::to_string(class_count)
				        + " classes but class_names_file has "
				        + std::to_string(nn_class_names_.size()));
			}

			const auto t_before_decode = Clock::now();

			const float scale_x =
			    static_cast<float>(frame.cols) / input_width;
			const float scale_y =
			    static_cast<float>(frame.rows) / input_height;

			const float* data =
			    reinterpret_cast<const float*>(output.data);

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

				if(objectness < nn_confidence_threshold_)
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

				const float confidence =
				    objectness * best_class_score;

				if(confidence < nn_confidence_threshold_)
					continue;

				const float center_x = candidate[0] * scale_x;
				const float center_y = candidate[1] * scale_y;
				const float width = candidate[2] * scale_x;
				const float height = candidate[3] * scale_y;

				cv::Rect box(
				    static_cast<int>(center_x - width * 0.5F),
				    static_cast<int>(center_y - height * 0.5F),
				    static_cast<int>(width),
				    static_cast<int>(height));

				box &= cv::Rect(0, 0, frame.cols, frame.rows);

				if(box.empty())
					continue;

				boxes.push_back(box);
				confidences.push_back(confidence);
				class_ids.push_back(best_class_id);
			}

			const auto t_after_decode = Clock::now();

			std::vector<int> kept_indices;
			cv::dnn::NMSBoxes(
			    boxes, confidences,
			    static_cast<float>(nn_confidence_threshold_),
			    static_cast<float>(nn_nms_threshold_),
			    kept_indices);

			for(const int idx: kept_indices)
			{
				detections.push_back(
				    {class_ids[idx], confidences[idx],
				     boxes[idx]});
			}

			std::sort(
			    detections.begin(), detections.end(),
			    [](const YoloDetection& a, const YoloDetection& b) {
				    return a.confidence > b.confidence;
			    });

			const auto t_end = Clock::now();

			auto to_ms = [](const auto& a,
			                const auto& b) -> double {
				return std::chrono::duration<double, std::milli>(
				           b - a)
				    .count();
			};

			local_timing.preprocess_ms =
			    to_ms(t_start, t_after_preprocess);
			local_timing.forward_ms =
			    to_ms(t_after_preprocess, t_after_forward);
			local_timing.decode_ms =
			    to_ms(t_before_decode, t_after_decode);
			local_timing.nms_ms =
			    to_ms(t_after_decode, t_end);
			local_timing.total_ms =
			    local_timing.preprocess_ms
			    + local_timing.forward_ms
			    + local_timing.decode_ms
			    + local_timing.nms_ms;

			if(timing != nullptr)
				*timing = local_timing;
		}
		catch(const cv::Exception& error)
		{
			ETEST_LOG_ERROR(
			    "VISION_YOLO",
			    std::string("OpenCV exception: ") + error.what());
		}
		catch(const std::exception& error)
		{
			ETEST_LOG_ERROR("VISION_YOLO",
			                std::string("exception: ") + error.what());
		}
		catch(...)
		{
			ETEST_LOG_ERROR("VISION_YOLO",
			                "unknown exception during inference");
		}

		return detections;
	}

	// ── 控制结果生成 ──

	VisionResult VisionProcessor::processYoloBall(
	    const cv::Mat& frame, YoloTiming* timing) noexcept
	{
		VisionResult result;
		result.target_type = "BALL";

		try
		{
			const auto detections = inferYolo(frame, timing);
			yolo_last_detections_ = detections;

			if(!nn_loaded_ || yolo_model_unhealthy_)
			{
				result.error_code = "MODEL_NOT_READY";
				return result;
			}

			// ── 选球 ──
			const YoloDetection* best = nullptr;

			if(!detections.empty())
			{
				if(!yolo_tracking_initialized_)
				{
					// 未初始化：选置信度最高的
					best = &detections[0];
				}
				else
				{
					// 跟踪模式：选距离上一帧最近的
					double best_dist = 1e9;
					for(const auto& d: detections)
					{
						cv::Point2f c = d.center();
						double dist = cv::norm(c - yolo_last_center_);
						if(dist < best_dist)
						{
							best_dist = dist;
							best = &d;
						}
					}

					// 最近候选仍超过最大跳变 → 拒绝
					if(best != nullptr)
					{
						cv::Point2f c = best->center();
						double dist = cv::norm(c - yolo_last_center_);
						constexpr double max_jump_px = 80.0;
						if(dist > max_jump_px)
							best = nullptr;
					}
				}
			}

			// ── 丢球处理 ──
			if(best == nullptr)
			{
				++yolo_lost_frames_;
				yolo_reacquire_confirm_ = 0;

				if(yolo_lost_frames_ == 1)
				{
					ETEST_LOG_WARN("VISION_YOLO",
					               "ball detection lost");
				}

				if(yolo_lost_frames_ > 3)
				{
					yolo_tracking_initialized_ = false;
					result.valid = false;
					result.error_code = "BALL_LOST";
				}
				else if(yolo_tracking_initialized_)
				{
					// 短暂保持：使用上一次有效检测的置信度
					result.valid = true;
					result.calibrated = yolo_zero_locked_;
					result.x = yolo_last_center_.x;
					result.y = yolo_last_center_.y;
					result.confidence =
					    yolo_last_valid_confidence_ * 0.5;
				}
				else
				{
					result.error_code = "BALL_LOST";
				}

				return result;
			}

			// ── 检测到球 ──
			yolo_lost_frames_ = 0;
			yolo_last_valid_confidence_ =
			    static_cast<double>(best->confidence);
			cv::Point2f center = best->center();
			yolo_last_center_ = center;

			if(!yolo_tracking_initialized_)
			{
				++yolo_reacquire_confirm_;
				if(yolo_reacquire_confirm_ < 2)
				{
					result.error_code = "BALL_LOST";
					return result;
				}
				yolo_tracking_initialized_ = true;
				yolo_filtered_x_ = center.x;
			}

			// ── 位置滤波 ──
			const double alpha = 0.45;
			yolo_filtered_x_ =
			    alpha * center.x + (1.0 - alpha) * yolo_filtered_x_;

			// ── 原点标定 ──
			if(!yolo_zero_locked_)
			{
				const double min_conf = 0.55;

				if(best->confidence >= min_conf)
				{
					if(!yolo_zero_samples_.empty())
					{
						cv::Point2f prev = yolo_zero_samples_.back();
						if(cv::norm(center - prev) > 50.0)
						{
							// 跳动过大，清空重新采集
							yolo_zero_samples_.clear();
							yolo_calib_frame_count_ = 0;
						}
					}

					yolo_zero_samples_.push_back(center);
					++yolo_calib_frame_count_;

					constexpr int required_samples = 12;
					if(static_cast<int>(yolo_zero_samples_.size())
					   >= required_samples)
					{
						// 计算 x/y 范围
						auto [min_x_it, max_x_it] = std::minmax_element(
						    yolo_zero_samples_.begin(),
						    yolo_zero_samples_.end(),
						    [](const cv::Point2f& a,
						       const cv::Point2f& b) {
							    return a.x < b.x;
						    });
						auto [min_y_it, max_y_it] = std::minmax_element(
						    yolo_zero_samples_.begin(),
						    yolo_zero_samples_.end(),
						    [](const cv::Point2f& a,
						       const cv::Point2f& b) {
							    return a.y < b.y;
						    });

						double x_range = max_x_it->x - min_x_it->x;
						double y_range = max_y_it->y - min_y_it->y;

						constexpr double max_jitter = 5.0;
						if(x_range <= max_jitter
						   && y_range <= max_jitter)
						{
							// 中位数原点
							std::vector<float> xs, ys;
							xs.reserve(yolo_zero_samples_.size());
							ys.reserve(yolo_zero_samples_.size());
							for(const auto& p: yolo_zero_samples_)
							{
								xs.push_back(p.x);
								ys.push_back(p.y);
							}
							std::sort(xs.begin(), xs.end());
							std::sort(ys.begin(), ys.end());

							yolo_zero_origin_.x = xs[xs.size() / 2];
							yolo_zero_origin_.y = ys[ys.size() / 2];
							yolo_zero_locked_ = true;

							ETEST_LOG_INFO(
							    "VISION_YOLO",
							    "zero position locked: x="
							        + std::to_string(
							            yolo_zero_origin_.x)
							        + ", y="
							        + std::to_string(
							            yolo_zero_origin_.y));
						}
						else
						{
							// 不稳定，清空
							yolo_zero_samples_.clear();
							yolo_calib_frame_count_ = 0;
						}
					}

					// 标定超时
					constexpr int max_wait = 90;
					if(yolo_calib_frame_count_ > max_wait
					   && !yolo_zero_locked_)
					{
						ETEST_LOG_WARN(
						    "VISION_YOLO",
						    "calibration timeout, restarting");
						yolo_zero_samples_.clear();
						yolo_calib_frame_count_ = 0;
					}
				}
			}

			// ── 物理换算 ──
			const double mm_per_pixel = 0.52;
			double offset_px = yolo_filtered_x_ - yolo_zero_origin_.x;

			int pos_0p1mm = static_cast<int>(
			    std::lround(offset_px * mm_per_pixel * 10.0));

			// ── 填充结果 ──
			result.valid = true;
			result.calibrated = yolo_zero_locked_;
			result.x = center.x;
			result.y = center.y;
			result.confidence = best->confidence;
			result.position_0p1mm = yolo_zero_locked_ ? pos_0p1mm : 0;
			result.offset_mm = yolo_zero_locked_ ? pos_0p1mm / 10 : 0;

			if(!yolo_zero_locked_)
			{
				result.error_code = "ZERO_CALIBRATING";
			}
			else
			{
				result.error_code.clear();
			}
		}
		catch(const cv::Exception& error)
		{
			ETEST_LOG_ERROR(
			    "VISION_YOLO",
			    std::string("ball processing cv:") + error.what());
			result.error_code = "YOLO_CV_EXCEPTION";
			++yolo_consecutive_errors_;
		}
		catch(const std::exception& error)
		{
			ETEST_LOG_ERROR(
			    "VISION_YOLO",
			    std::string("ball processing:") + error.what());
			result.error_code = "YOLO_EXCEPTION";
			++yolo_consecutive_errors_;
		}
		catch(...)
		{
			ETEST_LOG_ERROR("VISION_YOLO",
			                "unknown ball processing exception");
			result.error_code = "YOLO_UNKNOWN_EXCEPTION";
			++yolo_consecutive_errors_;
		}

		return result;
	}

	// ── YOLO 调试绘制 ──

	void VisionProcessor::drawYoloDebugInfo(
	    cv::Mat& frame, const VisionResult& result) noexcept
	{
		try
		{
			if(frame.empty())
				return;

			// 绘制所有检测框
			for(const auto& det: yolo_last_detections_)
			{
				cv::Scalar color(0, 255, 0);
				if(&det == &yolo_last_detections_[0])
					color = cv::Scalar(0, 255, 255); // 最佳候选

				cv::rectangle(frame, det.box, color, 2);
				cv::Point2f c = det.center();
				cv::drawMarker(frame,
				               cv::Point(static_cast<int>(c.x),
				                         static_cast<int>(c.y)),
				               color, cv::MARKER_CROSS, 10, 1);

				std::string label = std::to_string(static_cast<int>(
				                        det.confidence * 100))
				    + "%";
				cv::putText(frame, label,
				            cv::Point(det.box.x, det.box.y - 5),
				            cv::FONT_HERSHEY_SIMPLEX, 0.5, color, 1);
			}

			// 状态栏
			std::string phase_text;
			if(!yolo_zero_locked_)
				phase_text = "CALIB "
				    + std::to_string(yolo_zero_samples_.size()) + "/12";
			else if(result.valid)
				phase_text =
				    "OK offset=" + std::to_string(result.position_0p1mm)
				    + " (0.1mm)";
			else if(result.error_code == "BALL_LOST")
				phase_text = "LOST";
			else
				phase_text = result.error_code;

			cv::putText(frame, "YOLO: " + phase_text, cv::Point(10, 25),
			            cv::FONT_HERSHEY_SIMPLEX, 0.6,
			            cv::Scalar(200, 200, 200), 1);

			// 原点线
			if(yolo_zero_locked_)
			{
				int ox = static_cast<int>(yolo_zero_origin_.x);
				cv::line(frame, cv::Point(ox, 0),
				         cv::Point(ox, frame.rows),
				         cv::Scalar(255, 0, 0), 1);
				cv::putText(frame, "ZERO",
				            cv::Point(ox + 5, frame.rows - 10),
				            cv::FONT_HERSHEY_SIMPLEX, 0.4,
				            cv::Scalar(255, 0, 0), 1);
			}
		}
		catch(const cv::Exception& e)
		{
			ETEST_LOG_ERROR("VISION_YOLO",
			                std::string("draw:") + e.what());
		}
		catch(...)
		{
			ETEST_LOG_ERROR("VISION_YOLO", "draw unknown");
		}
	}

} // namespace etest::vision