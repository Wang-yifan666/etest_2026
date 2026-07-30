#pragma once

#include "vision/camera.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace etest::vision
{

	enum class CaptureWorkerState
	{
		STOPPED,
		STARTING,
		RUNNING,
		CAMERA_ERROR,
		FILE_EOF
	};

	struct FramePacket
	{
		cv::Mat frame;
		std::chrono::steady_clock::time_point received_at{};
		std::uint64_t sequence = 0;
	};

	class LatestFrameCapture
	{
	public:
		explicit LatestFrameCapture(Camera& camera);
		~LatestFrameCapture();

		LatestFrameCapture(const LatestFrameCapture&) = delete;
		LatestFrameCapture& operator=(const LatestFrameCapture&) =
		    delete;

		bool start();
		void stop() noexcept;

		// 非阻塞获取。
		// 只有 sequence 与 last_sequence 不同才返回 true。
		bool tryGetLatest(FramePacket& output,
		                  std::uint64_t last_sequence) const;

		// 等待很短时间（timeout 毫秒），不能无限等待。
		bool waitLatest(FramePacket& output,
		                std::uint64_t last_sequence,
		                std::chrono::milliseconds timeout);

		CaptureWorkerState state() const noexcept;

		std::uint64_t capturedFrames() const noexcept;
		std::uint64_t readFailures() const noexcept;

	private:
		void captureLoop() noexcept;

		Camera& camera_;

		std::atomic_bool running_{false};
		std::thread worker_;

		mutable std::mutex mutex_;
		std::condition_variable condition_;

		FramePacket latest_;
		CaptureWorkerState state_ = CaptureWorkerState::STOPPED;

		std::atomic_uint64_t captured_frames_{0};
		std::atomic_uint64_t read_failures_{0};
	};

} // namespace etest::vision