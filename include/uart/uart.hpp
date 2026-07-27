#pragma once

#include "core/config.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace etest
{

	enum class UartMessageType
	{
		OK,
		ERROR,
		WARNING,
		DONE,
		BOOT,
		KEY_EVENT,
		BLOCK_BEGIN,
		BLOCK_END,
		DATA,
		UNKNOWN
	};

	struct UartMessage
	{
		UartMessageType type = UartMessageType::UNKNOWN;
		std::string raw;
		std::string tag;

		// 不包含 tag，并保留空字段。
		// 例如 ERR,SAFETY,FAULT_ACTIVE,,DETAIL 中空 KEY 不会丢失。
		std::vector<std::string> fields;

		bool isCritical() const noexcept;
	};

	struct UartBlock
	{
		std::string tag;
		std::string seq;
		std::vector<UartMessage> lines;
		bool complete = false;
		std::string error;
	};

	enum class UartBlockResult
	{
		IGNORED,
		CONSUMED,
		COMPLETED,
		ABORTED
	};

	// 通用多行数据块组装器：STATUS_BEGIN...STATUS_END 等。
	// 异步 OK/ERR/WARN/DONE/BOOT/M000x 不会被吞入数据块。
	class UartBlockAssembler
	{
	public:
		explicit UartBlockAssembler(int timeout_ms = 1000) noexcept;

		UartBlockResult consume(const UartMessage& message,
		                        UartBlock& output) noexcept;

		UartBlockResult checkTimeout(UartBlock& output) noexcept;

		void reset() noexcept;
		bool active() const noexcept;

	private:
		int timeout_ms_ = 1000;
		bool active_ = false;
		std::string tag_;
		std::string seq_;
		std::vector<UartMessage> lines_;
		std::chrono::steady_clock::time_point started_at_{};
	};

	class Uart
	{
	public:
		explicit Uart(UartConfig config = {});
		~Uart();

		// 启动后台接收线程。首次打开失败不会让程序退出；
		// auto_reconnect=true 时持续尝试重连。
		bool start() noexcept;
		void stop() noexcept;

		// 单次打开/关闭接口，主要用于调试。
		bool open() noexcept;
		void close() noexcept;

		// 原始发送，不自动添加换行。
		bool send(const std::vector<std::uint8_t>& data) noexcept;
		bool send(const std::string& text) noexcept;

		// 协议命令发送：自动添加 \r\n，拒绝内嵌 CR/LF。
		bool sendLine(const std::string& line) noexcept;

		// 主线程从有界队列取消息。
		bool tryPop(UartMessage& message) noexcept;
		bool waitPop(UartMessage& message, int timeout_ms) noexcept;
		std::size_t pendingCount() const noexcept;

		bool isOpen() const noexcept;
		bool isRunning() const noexcept;

		static UartMessage parseLine(const std::string& line) noexcept;

		Uart(const Uart&) = delete;
		Uart& operator=(const Uart&) = delete;

	private:
		bool openUnlocked() noexcept;
		void closeUnlocked() noexcept;
		bool sendBytes(const std::uint8_t* data,
		               std::size_t size) noexcept;

		void receiveLoop() noexcept;
		void receiveIteration() noexcept;
		void processBytes(const char* data, std::size_t size) noexcept;
		void pushMessage(UartMessage message) noexcept;
		void sleepReconnect() const noexcept;

		static bool isCriticalType(UartMessageType type) noexcept;

		UartConfig config_;

		mutable std::mutex fd_mutex_;
		int fd_ = -1;

		std::atomic<bool> running_{false};
		std::atomic<bool> connected_{false};
		std::thread receive_thread_;

		std::mutex tx_mutex_;

		std::string rx_buffer_;
		bool discarding_overlong_line_ = false;

		mutable std::mutex queue_mutex_;
		std::condition_variable queue_cv_;
		std::deque<UartMessage> queue_;
	};

} // namespace etest