#include "stream/video_streamer.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <exception>
#include <cstring>
#include <iostream>
#include <sstream>
#include <utility>

#include <opencv2/imgproc.hpp>

namespace etest::stream
{

VideoStreamer::VideoStreamer(VideoStreamConfig config)
    : config_(std::move(config))
{
}

VideoStreamer::~VideoStreamer()
{
	stop();
}

bool VideoStreamer::start()
{
	if(!config_.enabled)
	{
		std::cout << "[VideoStreamer] disabled\n";
		return true;
	}

	if(running_.load())
		return true;

	if(config_.receiver_ip.empty() || config_.video_port <= 0
	   || config_.control_port <= 0 || config_.width <= 0
	   || config_.height <= 0 || config_.fps <= 0
	   || config_.bitrate_kbps <= 0)
	{
		std::cerr << "[VideoStreamer] invalid configuration\n";
		return false;
	}

	if(!openPipeline())
		return false;

	running_.store(true);
	worker_ = std::thread(&VideoStreamer::workerLoop, this);

	std::cout << "[VideoStreamer] video -> " << config_.receiver_ip << ":"
	          << config_.video_port << ", control -> "
	          << config_.receiver_ip << ":" << config_.control_port << "\n";
	return true;
}

void VideoStreamer::stop() noexcept
{
	const bool was_running = running_.exchange(false);

	{
		std::lock_guard<std::mutex> lock(frame_mutex_);
		frame_ready_ = false;
		latest_frame_.release();
	}

	frame_cv_.notify_all();

	if(worker_.joinable())
		worker_.join();

	if(writer_.isOpened())
		writer_.release();

	if(was_running)
		std::cout << "[VideoStreamer] stopped\n";
}

void VideoStreamer::submit(const cv::Mat& frame)
{
	if(!running_.load() || frame.empty())
		return;

	// 限制提交速率，避免摄像头 30 FPS 时向 25 FPS 编码器堆积帧。
	const auto now = std::chrono::steady_clock::now();
	const auto min_interval =
	    std::chrono::microseconds(1000000 / config_.fps);

	{
		std::lock_guard<std::mutex> lock(rate_mutex_);
		if(last_submit_time_.time_since_epoch().count() != 0
		   && now - last_submit_time_ < min_interval)
		{
			return;
		}
		last_submit_time_ = now;
	}

	cv::Mat output;
	const cv::Size target_size(config_.width, config_.height);

	if(frame.size() == target_size)
		frame.copyTo(output);
	else
		cv::resize(frame, output, target_size, 0.0, 0.0, cv::INTER_AREA);

	{
		std::lock_guard<std::mutex> lock(frame_mutex_);
		// 覆盖旧帧，只保留最新帧，绝不形成延迟队列。
		latest_frame_ = std::move(output);
		frame_ready_ = true;
	}

	frame_cv_.notify_one();
}

bool VideoStreamer::sendTestStart(std::uint32_t session_id,
                                  const std::string& mode) const noexcept
{
	const std::string payload =
	    "TEST_START|" + std::to_string(session_id) + "|"
	    + sanitizeField(mode);
	return sendControl(payload);
}

bool VideoStreamer::sendTestDone(std::uint32_t session_id,
                                 const std::string& mode,
                                 const std::string& result) const noexcept
{
	const std::string payload =
	    "TEST_DONE|" + std::to_string(session_id) + "|"
	    + sanitizeField(mode) + "|" + sanitizeField(result);
	return sendControl(payload);
}

bool VideoStreamer::running() const noexcept
{
	return running_.load();
}

bool VideoStreamer::openPipeline()
{
	const std::string pipeline = buildPipeline();

	writer_.open(pipeline,
	             cv::CAP_GSTREAMER,
	             0,
	             static_cast<double>(config_.fps),
	             cv::Size(config_.width, config_.height),
	             true);

	if(!writer_.isOpened())
	{
		std::cerr
		    << "[VideoStreamer] failed to open GStreamer pipeline.\n"
		    << "[VideoStreamer] Check: OpenCV GStreamer support and "
		       "gst-inspect-1.0 x264enc\n"
		    << "[VideoStreamer] pipeline: " << pipeline << "\n";
		return false;
	}

	return true;
}

std::string VideoStreamer::buildPipeline() const
{
	std::ostringstream pipeline;

	pipeline
	    << "appsrc is-live=true do-timestamp=true format=time "
	    << "! queue leaky=downstream max-size-buffers=1 "
	    << "! videoconvert "
	    << "! video/x-raw,format=I420 "
	    << "! x264enc "
	    << "tune=zerolatency "
	    << "speed-preset=ultrafast "
	    << "bitrate=" << config_.bitrate_kbps << " "
	    << "key-int-max=" << config_.fps << " "
	    << "bframes=0 "
	    << "byte-stream=true "
	    << "threads=2 "
	    << "! h264parse "
	    << "! rtph264pay "
	    << "pt=96 "
	    << "config-interval=1 "
	    << "mtu=1200 "
	    << "! udpsink "
	    << "host=" << config_.receiver_ip << " "
	    << "port=" << config_.video_port << " "
	    << "sync=false "
	    << "async=false";

	return pipeline.str();
}

void VideoStreamer::workerLoop() noexcept
{
	try
	{
		while(running_.load())
		{
			cv::Mat frame;

			{
				std::unique_lock<std::mutex> lock(frame_mutex_);
				frame_cv_.wait_for(
				    lock,
				    std::chrono::milliseconds(100),
				    [this] {
					    return !running_.load() || frame_ready_;
				    });

				if(!running_.load())
					break;

				if(!frame_ready_)
					continue;

				frame = std::move(latest_frame_);
				frame_ready_ = false;
			}

			if(!frame.empty() && writer_.isOpened())
				writer_.write(frame);
		}
	}
	catch(const cv::Exception& error)
	{
		std::cerr << "[VideoStreamer] OpenCV error: "
		          << error.what() << "\n";
	}
	catch(const std::exception& error)
	{
		std::cerr << "[VideoStreamer] error: "
		          << error.what() << "\n";
	}
	catch(...)
	{
		std::cerr << "[VideoStreamer] unknown worker error\n";
	}

	running_.store(false);
}

bool VideoStreamer::sendControl(const std::string& payload) const noexcept
{
	if(!config_.enabled)
		return false;

	const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
	if(fd < 0)
	{
		std::cerr << "[VideoStreamer] control socket failed: "
		          << std::strerror(errno) << "\n";
		return false;
	}

	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_port = htons(
	    static_cast<std::uint16_t>(config_.control_port));

	if(::inet_pton(AF_INET,
	               config_.receiver_ip.c_str(),
	               &address.sin_addr)
	   != 1)
	{
		std::cerr << "[VideoStreamer] invalid receiver IP: "
		          << config_.receiver_ip << "\n";
		::close(fd);
		return false;
	}

	bool any_success = false;

	// UDP 控制消息连续发送三份；接收端按 session_id 去重。
	for(int i = 0; i < 3; ++i)
	{
		const ssize_t sent =
		    ::sendto(fd,
		             payload.data(),
		             payload.size(),
		             0,
		             reinterpret_cast<const sockaddr*>(&address),
		             sizeof(address));

		if(sent == static_cast<ssize_t>(payload.size()))
			any_success = true;
	}

	::close(fd);

	if(!any_success)
	{
		std::cerr << "[VideoStreamer] control send failed: "
		          << payload << "\n";
	}

	return any_success;
}

std::string VideoStreamer::sanitizeField(const std::string& value)
{
	std::string result = value;

	for(char& ch: result)
	{
		if(ch == '|' || ch == '\r' || ch == '\n')
			ch = '_';
	}

	if(result.empty())
		result = "UNKNOWN";

	return result;
}

} // namespace etest::stream