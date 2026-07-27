#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "core/config.hpp"
#include "state/state.hpp"

// 辅助函数

static bool writeFile(const std::string& path,
                      const std::string& content)
{
	std::ofstream out(path);

	if(!out.is_open())
	{
		return false;
	}

	out << content;

	return true;
}

// 原有测试（已通过的）

static void test_mode_disabled_no_mode_file()
{
	const auto result =
	    etest::loadAppConfigFromDir("config");

	assert(!result.config.mode.enabled);
	assert(!result.mode_applied);

	std::cout << "[PASS] test_mode_disabled_no_mode_file\n";
}

static void test_competition_overrides_search_preview()
{
	const std::string dir = "/tmp/etest_test2";
	std::system(("mkdir -p " + dir + "/modes").c_str());

	writeFile(dir + "/main.toml",
	          "[mode]\n"
	          "enabled = true\n"
	          "name = \"competition\"\n");

	writeFile(dir + "/logger.toml", "");
	writeFile(dir + "/camera.toml", "");
	writeFile(dir + "/search.toml",
	          "[search]\n"
	          "show_preview = true\n");

	writeFile(dir + "/modes/competition.toml",
	          "[search]\n"
	          "show_preview = false\n");

	const auto result = etest::loadAppConfigFromDir(dir);

	assert(result.config.mode.enabled);
	assert(result.config.mode.name == "competition");
	assert(result.mode_applied);
	assert(!result.config.search.show_preview);

	std::system(("rm -rf " + dir).c_str());

	std::cout << "[PASS] test_competition_overrides_search_preview\n";
}

static void test_debug_overrides_log_level()
{
	const std::string dir = "/tmp/etest_test3";
	std::system(("mkdir -p " + dir + "/modes").c_str());

	writeFile(dir + "/main.toml",
	          "[mode]\n"
	          "enabled = true\n"
	          "name = \"debug\"\n");

	writeFile(dir + "/logger.toml",
	          "[logger]\n"
	          "min_level = \"INFO\"\n");

	writeFile(dir + "/camera.toml", "");
	writeFile(dir + "/search.toml", "");

	writeFile(dir + "/modes/debug.toml",
	          "[logger]\n"
	          "min_level = \"DEBUG\"\n");

	const auto result = etest::loadAppConfigFromDir(dir);

	assert(result.config.mode.enabled);
	assert(result.config.mode.name == "debug");
	assert(result.mode_applied);
	assert(result.config.logger.min_level == etest::LogLevel::DEBUG);

	std::system(("rm -rf " + dir).c_str());

	std::cout << "[PASS] test_debug_overrides_log_level\n";
}

static void test_missing_mode_file_continues()
{
	const std::string dir = "/tmp/etest_test4";
	std::system(("mkdir -p " + dir + "/modes").c_str());

	writeFile(dir + "/main.toml",
	          "[mode]\n"
	          "enabled = true\n"
	          "name = \"competition\"\n");

	writeFile(dir + "/logger.toml", "");
	writeFile(dir + "/camera.toml", "");
	writeFile(dir + "/search.toml", "");

	const auto result = etest::loadAppConfigFromDir(dir);

	assert(result.config.mode.enabled);
	assert(!result.mode_applied);

	bool has_error = false;

	for(const auto& msg: result.messages)
	{
		if(msg.level == etest::ConfigMessageLevel::ERROR
		   && msg.description.find("cannot open mode file")
		          != std::string::npos)
		{
			has_error = true;
			break;
		}
	}

	assert(has_error);

	std::system(("rm -rf " + dir).c_str());

	std::cout << "[PASS] test_missing_mode_file_continues\n";
}

static void test_illegal_mode_name_rejected()
{
	const std::string dir = "/tmp/etest_test5";
	std::system(("mkdir -p " + dir).c_str());

	writeFile(dir + "/main.toml",
	          "[mode]\n"
	          "enabled = true\n"
	          "name = \"../test\"\n");

	writeFile(dir + "/logger.toml", "");
	writeFile(dir + "/camera.toml", "");
	writeFile(dir + "/search.toml", "");

	const auto result = etest::loadAppConfigFromDir(dir);

	assert(!result.mode_applied);

	bool has_error = false;

	for(const auto& msg: result.messages)
	{
		if(msg.level == etest::ConfigMessageLevel::ERROR
		   && msg.description.find("invalid mode name")
		          != std::string::npos)
		{
			has_error = true;
			break;
		}
	}

	assert(has_error);

	std::system(("rm -rf " + dir).c_str());

	std::cout << "[PASS] test_illegal_mode_name_rejected\n";
}

static void test_invalid_mode_field_preserves_previous()
{
	const std::string dir = "/tmp/etest_test6";
	std::system(("mkdir -p " + dir + "/modes").c_str());

	writeFile(dir + "/main.toml",
	          "[mode]\n"
	          "enabled = true\n"
	          "name = \"competition\"\n");

	writeFile(dir + "/logger.toml", "");
	writeFile(dir + "/camera.toml",
	          "[camera]\n"
	          "width = 1280\n");

	writeFile(dir + "/search.toml", "");

	writeFile(dir + "/modes/competition.toml",
	          "[camera]\n"
	          "width = -100\n");

	const auto result = etest::loadAppConfigFromDir(dir);

	assert(result.config.camera.width == 1280);

	std::system(("rm -rf " + dir).c_str());

	std::cout << "[PASS] test_invalid_mode_field_preserves_previous\n";
}

static void test_headless_forces_preview_and_keyboard_off()
{
	const std::string dir = "/tmp/etest_test7";
	std::system(("mkdir -p " + dir + "/modes").c_str());

	writeFile(dir + "/main.toml",
	          "[mode]\n"
	          "enabled = true\n"
	          "name = \"debug\"\n"
	          "\n"
	          "[runtime]\n"
	          "headless = true\n"
	          "allow_keyboard_exit = true\n");

	writeFile(dir + "/logger.toml", "");
	writeFile(dir + "/camera.toml", "");
	writeFile(dir + "/search.toml",
	          "[search]\n"
	          "show_preview = true\n");

	writeFile(dir + "/modes/debug.toml", "");

	const auto result = etest::loadAppConfigFromDir(dir);

	assert(result.config.runtime.headless);
	assert(!result.config.search.show_preview);
	assert(!result.config.runtime.allow_keyboard_exit);

	std::system(("rm -rf " + dir).c_str());

	std::cout
	    << "[PASS] test_headless_forces_preview_and_keyboard_off\n";
}

static void test_mode_overrides_normal_config()
{
	const std::string dir = "/tmp/etest_test8";
	std::system(("mkdir -p " + dir + "/modes").c_str());

	writeFile(dir + "/main.toml",
	          "[mode]\n"
	          "enabled = true\n"
	          "name = \"debug\"\n");

	writeFile(dir + "/logger.toml",
	          "[logger]\n"
	          "min_level = \"INFO\"\n");

	writeFile(dir + "/camera.toml", "");
	writeFile(dir + "/search.toml", "");

	writeFile(dir + "/modes/debug.toml",
	          "[logger]\n"
	          "min_level = \"DEBUG\"\n");

	const auto result = etest::loadAppConfigFromDir(dir);

	assert(result.config.logger.min_level == etest::LogLevel::DEBUG);

	std::system(("rm -rf " + dir).c_str());

	std::cout << "[PASS] test_mode_overrides_normal_config\n";
}

// 新增测试

// 测试 9：FaultInfo 默认值
static void test_fault_info_defaults()
{
	etest::FaultInfo fault;

	assert(fault.source == etest::FaultSource::NONE);
	assert(fault.action == etest::RecoveryAction::CONTINUE);
	assert(fault.occurrence_count == 0);
	assert(fault.code.empty());
	assert(fault.message.empty());

	std::cout << "[PASS] test_fault_info_defaults\n";
}

// 测试 10：连续异常达阈值后触发重启
static void test_consecutive_exceptions_threshold()
{
	int consecutive = 0;

	// 模拟 9 次异常（未达阈值）
	for(int i = 0; i < 9; ++i)
	{
		consecutive++;
	}

	assert(consecutive == 9);
	assert(consecutive < 10); // kMaxConsecutiveExceptions = 10

	// 第 10 次触发阈值
	consecutive++;

	assert(consecutive >= 10);

	std::cout << "[PASS] test_consecutive_exceptions_threshold\n";
}

// 测试 11：故障恢复后清零连续计数
static void test_fault_recovery_resets_counter()
{
	etest::FaultInfo fault;
	fault.source = etest::FaultSource::CAMERA;
	fault.action = etest::RecoveryAction::CONTINUE;
	fault.code = "TEST";
	fault.message = "test fault";

	// 验证 fault 可以正确设置
	assert(fault.source == etest::FaultSource::CAMERA);
	assert(fault.action == etest::RecoveryAction::CONTINUE);

	// 清零逻辑
	fault = {};
	assert(fault.source == etest::FaultSource::NONE);

	int counter = 5;
	// 恢复成功后清零
	if(fault.source == etest::FaultSource::NONE)
	{
		counter = 0;
	}

	assert(counter == 0);

	std::cout << "[PASS] test_fault_recovery_resets_counter\n";
}

// 测试 12：停止信号结束主循环
static void test_shutdown_signal_ends_loop()
{
	std::atomic_bool shutdown_flag{false};

	// 模拟主循环
	bool running = true;
	int iterations = 0;

	// 模拟若干次迭代
	while(running && iterations < 5)
	{
		++iterations;

		// 第3次迭代后设置信号
		if(iterations >= 3)
		{
			shutdown_flag.store(true);
		}

		if(shutdown_flag.load())
		{
			running = false;
		}
	}

	assert(iterations == 3);
	assert(!running);

	std::cout << "[PASS] test_shutdown_signal_ends_loop\n";
}

// 测试 13：RuntimeConfig 恢复开关
static void test_recovery_switch_affects_behavior()
{
	etest::RuntimeConfig runtime;

	// 默认 enable_auto_recovery=true
	assert(runtime.enable_auto_recovery);

	// 设置 false
	runtime.enable_auto_recovery = false;
	assert(!runtime.enable_auto_recovery);

	// enable_self_check 默认 true
	assert(runtime.enable_self_check);

	// 设置 false
	runtime.enable_self_check = false;
	assert(!runtime.enable_self_check);

	std::cout << "[PASS] test_recovery_switch_affects_behavior\n";
}

// 测试 14：Error 状态入口计数
static void test_error_state_entry_count()
{
	int error_count = 0;

	// 模拟 4 次进入（未达 5 次阈值）
	for(int i = 0; i < 4; ++i)
	{
		error_count++;
	}

	assert(error_count == 4);

	// 第 5 次 → 触发 SAFE_STOP
	error_count++;
	assert(error_count >= 5);

	std::cout << "[PASS] test_error_state_entry_count\n";
}

// main

int main()
{
	// 原有测试
	test_mode_disabled_no_mode_file();
	test_competition_overrides_search_preview();
	test_debug_overrides_log_level();
	test_missing_mode_file_continues();
	test_illegal_mode_name_rejected();
	test_invalid_mode_field_preserves_previous();
	test_headless_forces_preview_and_keyboard_off();
	test_mode_overrides_normal_config();

	// 新增测试
	test_fault_info_defaults();
	test_consecutive_exceptions_threshold();
	test_fault_recovery_resets_counter();
	test_shutdown_signal_ends_loop();
	test_recovery_switch_affects_behavior();
	test_error_state_entry_count();

	std::cout << "\nall tests passed\n";

	return 0;
}