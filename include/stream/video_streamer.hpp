#pragma once

#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>
#include <string>
#include <thread>

namespace etest::stream
{

struct VideoStreamConfig
{
	bool enabled = true;
	std::string receiver_ip = "192.168.50.1";
	int video_port = 5600;
	int control_port = 5601;
	int width = 960;
	int height = 480;
	int fps = 25;
	int bitrate_kbps = 2500;
};

class VideoStreamer final
{
public:
	explicit VideoStreamer(VideoStreamConfig config = {});
	~VideoStreamer();

	bool start();
	void stop() noexcept;

	// 只保留最新帧。编码和网络发送在后台线程完成，不阻塞视觉主循环。
	void submit(const cv::Mat& frame);

	bool sendTestStart(std::uint32_t session_id,
	                   const std::string& mode) const noexcept;

	bool sendTestDone(std::uint32_t session_id,
	                  const std::string& mode,
	                  const std::string& result) const noexcept;

	bool running() const noexcept;

	VideoStreamer(const VideoStreamer&) = delete;
	VideoStreamer& operator=(const VideoStreamer&) = delete;

private:
	bool openPipeline();
	std::string buildPipeline() const;
	void workerLoop() noexcept;

	bool sendControl(const std::string& payload) const noexcept;
	static std::string sanitizeField(const std::string& value);

	VideoStreamConfig config_;

	std::atomic_bool running_{false};
	std::thread worker_;

	mutable std::mutex frame_mutex_;
	std::condition_variable frame_cv_;
	cv::Mat latest_frame_;
	bool frame_ready_ = false;

	std::mutex rate_mutex_;
	std::chrono::steady_clock::time_point last_submit_time_{};

	cv::VideoWriter writer_;
};

} // namespace etest::stream