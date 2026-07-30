#ifdef ETEST_HAS_NCNN

#include "vision/yolo_backend.hpp"
#include "core/logger.hpp"

#include <net.h>

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

	class NcnnYoloBackend final: public IYoloBackend
	{
	public:
		bool load(const std::string& model_prefix,
		          const YoloBackendConfig& config,
		          std::string& error) noexcept override;

		bool forward(const cv::Mat& frame, YoloRawOutput& output,
		             YoloTiming* timing,
		             std::string& error) noexcept override;

		bool ready() const noexcept override;
		const char* name() const noexcept override
		{
			return "ncnn";
		}

	private:
		ncnn::Net net_;
		YoloBackendConfig config_;
		bool ready_ = false;
		bool shape_logged_ = false;
		int expected_columns_ = 6;
	};

	bool NcnnYoloBackend::load(const std::string& model_prefix,
	                           const YoloBackendConfig& config,
	                           std::string& error) noexcept
	{
		// model_prefix is the param file path like
		// "model/ncnn_640x640/best_640x640_fp32.ncnn.param"
		// bin path is derived by replacing .param → .bin
		net_.clear();

		net_.opt.num_threads = config.num_threads;
		net_.opt.use_vulkan_compute = config.use_vulkan;
		net_.opt.use_fp16_packed = config.use_fp16_storage;
		net_.opt.use_fp16_storage = config.use_fp16_storage;
		net_.opt.use_fp16_arithmetic = config.use_fp16_arithmetic;

		const std::string param_path = model_prefix;
		std::string bin_path = model_prefix;

		// 替换 .param → .bin
		{
			const std::string param_ext = ".param";
			const std::string bin_ext = ".bin";
			auto pos = bin_path.rfind(param_ext);
			if(pos != std::string::npos
			   && pos + param_ext.size() == bin_path.size())
			{
				bin_path.replace(pos, param_ext.size(), bin_ext);
			}
			else
			{
				bin_path += ".bin";
			}
		}

		const int param_result = net_.load_param(param_path.c_str());
		if(param_result != 0)
		{
			error = "load_param failed: " + std::to_string(param_result)
			    + " path=" + param_path;
			return false;
		}

		const int model_result = net_.load_model(bin_path.c_str());
		if(model_result != 0)
		{
			error = "load_model failed: " + std::to_string(model_result)
			    + " path=" + bin_path;
			return false;
		}

		config_ = config;
		ready_ = true;
		shape_logged_ = false;

		ETEST_LOG_INFO(
		    "NCNN_BACKEND",
		    "model loaded: " + model_prefix
		        + " input=" + std::to_string(config_.input_width) + "x"
		        + std::to_string(config_.input_height) + " threads="
		        + std::to_string(config_.num_threads) + " fp16_storage="
		        + std::to_string(config_.use_fp16_storage)
		        + " fp16_arithmetic="
		        + std::to_string(config_.use_fp16_arithmetic)
		        + " vulkan=" + std::to_string(config_.use_vulkan));

		return true;
	}

	bool NcnnYoloBackend::forward(const cv::Mat& frame,
	                              YoloRawOutput& output,
	                              YoloTiming* timing,
	                              std::string& error) noexcept
	{
		YoloTiming local_timing;
		const auto t_start = Clock::now();

		try
		{
			if(!ready_)
			{
				error = "backend not ready";
				return false;
			}

			if(frame.empty())
			{
				error = "empty frame";
				return false;
			}

			// ── 预处理：与 OpenCV blobFromImage 行为一致 ──
			cv::Mat contiguous;
			if(frame.isContinuous())
			{
				contiguous = frame;
			}
			else
			{
				contiguous = frame.clone();
			}

			ncnn::Mat input = ncnn::Mat::from_pixels_resize(
			    contiguous.data, ncnn::Mat::PIXEL_BGR2RGB,
			    contiguous.cols, contiguous.rows, config_.input_width,
			    config_.input_height);

			const float norm_values[3] = {
			    1.0F / 255.0F,
			    1.0F / 255.0F,
			    1.0F / 255.0F,
			};

			input.substract_mean_normalize(nullptr, norm_values);

			const auto t_after_preprocess = Clock::now();

			// ── 前向 ──
			ncnn::Extractor extractor = net_.create_extractor();
			extractor.set_light_mode(true);

			int result =
			    extractor.input(config_.input_blob.c_str(), input);

			if(result != 0)
			{
				error =
				    "extractor.input failed: " + std::to_string(result);
				return false;
			}

			ncnn::Mat ncnn_output;
			result = extractor.extract(config_.output_blob.c_str(),
			                           ncnn_output);

			if(result != 0)
			{
				error = "extractor.extract failed: "
				    + std::to_string(result);
				return false;
			}

			const auto t_after_forward = Clock::now();

			// ── 形状记录（仅首帧）──
			if(!shape_logged_)
			{
				shape_logged_ = true;
				ETEST_LOG_INFO(
				    "NCNN_BACKEND",
				    "output dims=" + std::to_string(ncnn_output.dims)
				        + " w=" + std::to_string(ncnn_output.w)
				        + " h=" + std::to_string(ncnn_output.h) + " c="
				        + std::to_string(ncnn_output.c) + " elempack="
				        + std::to_string(ncnn_output.elempack)
				        + " elemsize="
				        + std::to_string(ncnn_output.elemsize));
			}

			// ── 输出适配 ──
			if(ncnn_output.dims == 2)
			{
				if(ncnn_output.w == expected_columns_)
				{
					// [h, w] → rows=h, columns=w
					output.rows = ncnn_output.h;
					output.columns = ncnn_output.w;
				}
				else if(ncnn_output.h == expected_columns_)
				{
					// [w, h] → 需要转置
					output.rows = ncnn_output.w;
					output.columns = ncnn_output.h;
				}
				else
				{
					error = "unexpected NCNN output shape: w="
					    + std::to_string(ncnn_output.w)
					    + " h=" + std::to_string(ncnn_output.h)
					    + " expected_columns="
					    + std::to_string(expected_columns_);
					return false;
				}
			}
			else
			{
				error = "unsupported NCNN output dims: "
				    + std::to_string(ncnn_output.dims);
				return false;
			}

			// ── 复制到 vector<float> ──
			const std::size_t total =
			    static_cast<std::size_t>(output.rows)
			    * static_cast<std::size_t>(output.columns);

			output.data.resize(total);

			// 如果形状与预期布局一致（h=rows, w=columns），直接复制
			if(ncnn_output.w == expected_columns_
			   && ncnn_output.h == output.rows
			   && ncnn_output.w == output.columns)
			{
				std::memcpy(output.data.data(), ncnn_output.data,
				            total * sizeof(float));
			}
			else
			{
				// 转置 [w, h] → [h, w]
				const float* src =
				    static_cast<const float*>(ncnn_output.data);
				for(int r = 0; r < output.rows; ++r)
				{
					for(int c = 0; c < output.columns; ++c)
					{
						output.data[r * output.columns + c] =
						    src[c * output.rows + r];
					}
				}
			}

			// ── 计时 ──
			local_timing.preprocess_ms =
			    toMs(t_start, t_after_preprocess);
			local_timing.forward_ms =
			    toMs(t_after_preprocess, t_after_forward);
			local_timing.decode_ms = 0.0;
			local_timing.nms_ms = 0.0;
			local_timing.total_ms =
			    local_timing.preprocess_ms + local_timing.forward_ms;

			if(timing != nullptr)
				*timing = local_timing;

			return true;
		}
		catch(const std::exception& e)
		{
			error = std::string("exception: ") + e.what();
			return false;
		}
		catch(...)
		{
			error = "unknown exception";
			return false;
		}
	}

	bool NcnnYoloBackend::ready() const noexcept
	{
		return ready_;
	}

	// ── 工厂函数 ──

	std::unique_ptr<IYoloBackend> createNcnnBackend(
	    const std::string& param_path,
	    const YoloBackendConfig& config) noexcept
	{
		auto backend = std::make_unique<NcnnYoloBackend>();
		std::string error;
		if(!backend->load(param_path, config, error))
		{
			ETEST_LOG_ERROR("NCNN_BACKEND",
			                "factory load failed: " + error);
			return nullptr;
		}
		return backend;
	}

} // namespace etest::vision

#endif // ETEST_HAS_NCNN