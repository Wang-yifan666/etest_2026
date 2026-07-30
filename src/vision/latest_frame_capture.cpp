#include "vision/latest_frame_capture.hpp"
#include "core/logger.hpp"

#include <chrono>
#include <thread>
#include <utility>

namespace etest::vision
{

	LatestFrameCapture::LatestFrameCapture(Camera& camera):
	camera_(camera)
	{
	}

	LatestFrameCapture::~LatestFrameCapture()
	{
		stop();
	}

	bool LatestFrameCapture::start()
	{
		if(running_.load())
		{
			ETEST_LOG_WARN("LATEST_CAPTURE", "already running");
			return false;
		}

		if(!camera_.isOpened())
		{
			ETEST_LOG_ERROR("LATEST_CAPTURE", "camera not opened");
			return false;
		}

		{
			std::lock_guard<std::mutex> lock(mutex_);
			state_ = CaptureWorkerState::STARTING;
		}

		running_.store(true);
		worker_ = std::thread(&LatestFrameCapture::captureLoop, this);

		ETEST_LOG_INFO("LATEST_CAPTURE", "capture worker started");
		return true;
	}

	void LatestFrameCapture::stop() noexcept
	{
		if(!running_.load())
		{
			return;
		}

		running_.store(false);

		// 唤醒可能在等待 condition_variable 的采集线程
		condition_.notify_all();

		if(worker_.joinable())
		{
			worker_.join();
		}

		ETEST_LOG_INFO("LATEST_CAPTURE", "capture worker stopped");
	}

	bool LatestFrameCapture::tryGetLatest(
	    FramePacket& output, std::uint64_t last_sequence) const
	{
		std::lock_guard<std::mutex> lock(mutex_);

		if(latest_.sequence == last_sequence || latest_.sequence == 0)
		{
			return false;
		}

		output = latest_;
		return true;
	}

	bool LatestFrameCapture::waitLatest(
	    FramePacket& output, std::uint64_t last_sequence,
	    std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(mutex_);

		const bool has_new =
		    condition_.wait_for(lock, timeout, [this, last_sequence] {
			    return latest_.sequence != last_sequence
			        && latest_.sequence != 0;
		    });

		if(has_new)
		{
			output = latest_;
			return true;
		}

		return false;
	}

	CaptureWorkerState LatestFrameCapture::state() const noexcept
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return state_;
	}

	std::uint64_t LatestFrameCapture::capturedFrames() const noexcept
	{
		return captured_frames_.load();
	}

	std::uint64_t LatestFrameCapture::readFailures() const noexcept
	{
		return read_failures_.load();
	}

	void LatestFrameCapture::captureLoop() noexcept
	{
		{
			std::lock_guard<std::mutex> lock(mutex_);
			state_ = CaptureWorkerState::RUNNING;
		}

		int consecutive_failures = 0;
		constexpr int kMaxConsecutiveFailures = 3;
		constexpr auto kFailureSleep = std::chrono::milliseconds(10);

		while(running_.load())
		{
			cv::Mat captured;

			if(!camera_.read(captured))
			{
				++consecutive_failures;
				read_failures_.fetch_add(1);

				if(consecutive_failures >= kMaxConsecutiveFailures)
				{
					std::lock_guard<std::mutex> lock(mutex_);
					state_ = CaptureWorkerState::CAMERA_ERROR;
					condition_.notify_all();
					ETEST_LOG_ERROR(
					    "LATEST_CAPTURE",
					    "capture worker failed after "
					        + std::to_string(consecutive_failures)
					        + " consecutive failures");
					break;
				}

				std::this_thread::sleep_for(kFailureSleep);
				continue;
			}

			consecutive_failures = 0;

			FramePacket packet;
			packet.frame = std::move(captured);
			packet.received_at = std::chrono::steady_clock::now();
			packet.sequence = captured_frames_.fetch_add(1) + 1;

			{
				std::lock_guard<std::mutex> lock(mutex_);

				// 单槽覆盖：新帧直接替换旧帧
				latest_ = std::move(packet);
			}

			condition_.notify_one();
		}

		{
			std::lock_guard<std::mutex> lock(mutex_);

			if(state_ == CaptureWorkerState::RUNNING)
			{
				state_ = CaptureWorkerState::STOPPED;
			}
		}

		condition_.notify_all();
	}

} // namespace etest::vision