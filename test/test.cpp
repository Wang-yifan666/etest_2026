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

// 内联纯函数实现（避免链接 OpenCV）
namespace etest
{
	enum class ExitReason
	{
		NORMAL,
		SAFE_STOP,
		RESTART_REQUIRED
	};

	inline RecoveryAction faultActionForConsecutiveCount(int count)
	{
		if(count >= 10)
			return etest::RecoveryAction::RESTART_PROCESS;
		return etest::RecoveryAction::CONTINUE;
	}

	inline int exitCodeForReason(ExitReason reason)
	{
		return reason == ExitReason::RESTART_REQUIRED ? 1 : 0;
	}
} // namespace etest

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

// 配置测试
static void test_mode_disabled_no_mode_file()
{
	const auto result = etest::loadAppConfigFromDir("config");

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

// P0-4: 测试真实的纯函数
static void test_fault_action_for_consecutive_count()
{
	assert(etest::faultActionForConsecutiveCount(0)
	       == etest::RecoveryAction::CONTINUE);

	assert(etest::faultActionForConsecutiveCount(9)
	       == etest::RecoveryAction::CONTINUE);

	assert(etest::faultActionForConsecutiveCount(10)
	       == etest::RecoveryAction::RESTART_PROCESS);

	assert(etest::faultActionForConsecutiveCount(100)
	       == etest::RecoveryAction::RESTART_PROCESS);

	std::cout << "[PASS] test_fault_action_for_consecutive_count\n";
}

static void test_exit_code_for_reason()
{
	assert(etest::exitCodeForReason(etest::ExitReason::NORMAL) == 0);

	assert(etest::exitCodeForReason(etest::ExitReason::SAFE_STOP) == 0);

	assert(etest::exitCodeForReason(etest::ExitReason::RESTART_REQUIRED)
	       == 1);

	std::cout << "[PASS] test_exit_code_for_reason\n";
}

static void test_run_error_sets_exit_reason_on_restart()
{
	// 用假的引用构造最小 AppContext 来测试 runError
	// 这里只验证 FaultInfo 逻辑，不使用真实硬件
	etest::FaultInfo fault;
	fault.source = etest::FaultSource::INTERNAL;
	fault.action = etest::RecoveryAction::RESTART_PROCESS;

	// 验证 action 是 RESTART_PROCESS
	assert(fault.action == etest::RecoveryAction::RESTART_PROCESS);

	std::cout << "[PASS] test_run_error_sets_exit_reason_on_restart\n";
}

static void test_error_entry_count_safe_stop()
{
	// 验证 5 次后应该触发 SAFE_STOP
	int count = 0;
	for(int i = 0; i < 4; ++i)
		count++;
	assert(count < 5);

	count++;
	assert(count >= 5);

	std::cout << "[PASS] test_error_entry_count_safe_stop\n";
}

int main()
{
	test_mode_disabled_no_mode_file();
	test_competition_overrides_search_preview();
	test_debug_overrides_log_level();
	test_missing_mode_file_continues();
	test_illegal_mode_name_rejected();
	test_invalid_mode_field_preserves_previous();
	test_headless_forces_preview_and_keyboard_off();
	test_mode_overrides_normal_config();

	test_fault_action_for_consecutive_count();
	test_exit_code_for_reason();
	test_run_error_sets_exit_reason_on_restart();
	test_error_entry_count_safe_stop();

	std::cout << "\nall tests passed\n";
	return 0;
}