#include "vision/yolo_backend.hpp"

#include "core/logger.hpp"

#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>

#include <chrono>
#include <cstring>
#include <string>

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

	class OpenCvYoloBackend final: public IYoloBackend
	{
	public:
		bool load(const std::string& model_path,
		          const YoloBackendConfig& config,
		          std::string& error) noexcept override
		{
			try
			{
				net_ = cv::dnn::readNetFromONNX(model_path);

				if(net_.empty())
				{
					error = "readNetFromONNX returned empty net";
					return false;
				}

				net_.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
				net_.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);

				config_ = config;
				ready_ = true;

				// 缓存输出层名称（与现有 loadNnModel 行为一致）
				output_names_ = net_.getUnconnectedOutLayersNames();

				ETEST_LOG_INFO(
				    "OPENCV_BACKEND",
				    "model loaded: " + model_path + " input="
				        + std::to_string(config_.input_width) + "x"
				        + std::to_string(config_.input_height)
				        + " threads="
				        + std::to_string(config_.num_threads));

				return true;
			}
			catch(const cv::Exception& e)
			{
				error = std::string("cv exception: ") + e.what();
				ready_ = false;
				return false;
			}
			catch(const std::exception& e)
			{
				error = std::string("exception: ") + e.what();
				ready_ = false;
				return false;
			}
		}

		bool forward(const cv::Mat& frame, YoloRawOutput& output,
		             YoloTiming* timing,
		             std::string& error) noexcept override
		{
			YoloTiming local_timing;
			const auto t_start = Clock::now();

			try
			{
				if(!ready_ || net_.empty())
				{
					error = "backend not ready";
					return false;
				}

				if(frame.empty())
				{
					error = "empty frame";
					return false;
				}

					// ── 预处理 ──
				const int source_width = frame.cols;
				const int source_height = frame.rows;

				cv::Mat blob;

				if(config_.resize_mode == ResizeMode::LETTERBOX)
				{
					// Letterbox：保持宽高比缩放 + 补灰边
					const float scale = std::min(
					    static_cast<float>(config_.input_width)
					        / static_cast<float>(source_width),
					    static_cast<float>(config_.input_height)
					        / static_cast<float>(source_height));

					const int resized_width = std::max(
					    1,
					    static_cast<int>(
					        std::round(source_width * scale)));

					const int resized_height = std::max(
					    1,
					    static_cast<int>(
					        std::round(source_height * scale)));

					cv::Mat resized;
					cv::resize(frame, resized,
					           cv::Size(resized_width,
					                    resized_height));

					const int padding_left =
					    (config_.input_width - resized_width) / 2;

					const int padding_top =
					    (config_.input_height - resized_height)
					    / 2;

					const cv::Scalar gray(114, 114, 114);
					cv::copyMakeBorder(
					    resized, blob,
					    padding_top,
					    config_.input_height - resized_height
					        - padding_top,
					    padding_left,
					    config_.input_width - resized_width
					        - padding_left,
					    cv::BORDER_CONSTANT, gray);

					// BGR→RGB, HWC→CHW, normalize
					cv::Mat rgb;
					cv::cvtColor(blob, rgb, cv::COLOR_BGR2RGB);
					rgb.convertTo(rgb, CV_32F, 1.0 / 255.0);
					cv::dnn::blobFromImage(rgb, blob);

					// 记录变换参数
					output.transform.mode =
					    ResizeMode::LETTERBOX;
					output.transform.source_width =
					    source_width;
					output.transform.source_height =
					    source_height;
					output.transform.input_width =
					    config_.input_width;
					output.transform.input_height =
					    config_.input_height;
					output.transform.uniform_scale = scale;
					output.transform.padding_left =
					    padding_left;
					output.transform.padding_top =
					    padding_top;
				}
				else
				{
					// STRETCH：直接拉伸（现有行为）
					blob = cv::dnn::blobFromImage(
					    frame, 1.0 / 255.0,
					    cv::Size(config_.input_width,
					             config_.input_height),
					    cv::Scalar(),
					    true,  // BGR → RGB
					    false  // 不裁剪
					);

					// 记录变换参数
					output.transform.mode =
					    ResizeMode::STRETCH;
					output.transform.source_width =
					    source_width;
					output.transform.source_height =
					    source_height;
					output.transform.input_width =
					    config_.input_width;
					output.transform.input_height =
					    config_.input_height;
					output.transform.scale_x =
					    static_cast<float>(
					        config_.input_width)
					    / static_cast<float>(source_width);
					output.transform.scale_y =
					    static_cast<float>(
					        config_.input_height)
					    / static_cast<float>(source_height);
				}

				const auto t_after_preprocess = Clock::now();

				net_.setInput(blob);

				std::vector<cv::Mat> outputs;
				net_.forward(outputs, output_names_);

				const auto t_after_forward = Clock::now();

				if(outputs.empty())
				{
					error = "forward returned empty outputs";
					return false;
				}

				// 取第一个输出
				const cv::Mat& cv_output = outputs.front();

				// 适配形状
				if(cv_output.dims != 3)
				{
					error = "unsupported output dims: "
					    + std::to_string(cv_output.dims);
					return false;
				}

				// OpenCV 输出形状: [1, N, 6] → rows=N, columns=6
				output.rows = cv_output.size[1];
				output.columns = cv_output.size[2];

				if(output.columns < 6)
				{
					error = "invalid column count: "
					    + std::to_string(output.columns);
					return false;
				}

				// 复制到 vector<float>
				const std::size_t total =
				    static_cast<std::size_t>(output.rows)
				    * static_cast<std::size_t>(output.columns);

				output.data.resize(total);
				std::memcpy(output.data.data(), cv_output.data,
				            total * sizeof(float));

				// ── 计时 ──
				local_timing.preprocess_ms =
				    toMs(t_start, t_after_preprocess);
				local_timing.forward_ms =
				    toMs(t_after_preprocess, t_after_forward);
				local_timing.decode_ms = 0.0; // 后端不负责解码
				local_timing.nms_ms = 0.0;
				local_timing.total_ms = local_timing.preprocess_ms
				    + local_timing.forward_ms;

				if(timing != nullptr)
					*timing = local_timing;

				return true;
			}
			catch(const cv::Exception& e)
			{
				error = std::string("cv exception: ") + e.what();
				return false;
			}
			catch(const std::exception& e)
			{
				error = std::string("exception: ") + e.what();
				return false;
			}
		}

		bool ready() const noexcept override
		{
			return ready_;
		}

		const char* name() const noexcept override
		{
			return "opencv";
		}

	private:
		cv::dnn::Net net_;
		std::vector<std::string> output_names_;
		YoloBackendConfig config_;
		bool ready_ = false;
	};

	// ── 工厂函数 ──

	std::unique_ptr<IYoloBackend> createOpenCvBackend() noexcept
	{
		return std::make_unique<OpenCvYoloBackend>();
	}

} // namespace etest::vision