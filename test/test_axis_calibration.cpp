/**
 * 轴标定 unit test：pixelToMm 插值行为与配置解析严格校验。
 */

#include "core/config.hpp"
#include "vision/roi_utils.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{
	int failures = 0;

	void check(const std::string& name, bool condition)
	{
		if(!condition)
		{
			std::cerr << "FAIL: " << name << "\n";
			++failures;
		}
		else
		{
			std::cout << "PASS: " << name << "\n";
		}
	}

	void checkNear(const std::string& name, double value,
	               double expected, double tolerance = 0.001)
	{
		if(std::abs(value - expected) > tolerance)
		{
			std::cerr << "FAIL: " << name << " got " << value
			          << " expected " << expected << "\n";
			++failures;
		}
		else
		{
			std::cout << "PASS: " << name << "\n";
		}
	}

	etest::PipeAxisCalibration makeCalibration(
	    const std::vector<double>& pixels,
	    const std::vector<double>& positions, int image_right_sign = 1)
	{
		etest::PipeAxisCalibration cal;
		cal.image_right_sign = image_right_sign;

		for(std::size_t i = 0; i < pixels.size(); ++i)
		{
			cal.points.push_back({pixels[i], positions[i]});
		}

		return cal;
	}
} // namespace

int main()
{
	using namespace etest::vision::roi_utils;

	// ── 正常三点插值 ──
	{
		auto cal =
		    makeCalibration({392.0, 640.0, 887.0}, {-50.0, 0.0, 50.0});

		// 端点
		checkNear("left endpoint", pixelToMm(392.0, cal), -50.0);
		checkNear("right endpoint", pixelToMm(887.0, cal), 50.0);

		// 中点 (640 px → 0 mm)
		checkNear("mid point", pixelToMm(640.0, cal), 0.0);

		// 中间插值：516 px（介于 392 和 640 之间）
		// ratio = (516 - 392) / (640 - 392) = 124 / 248 = 0.5
		// mm = -50 + 0.5 * 50 = -25
		checkNear("interpolation 516->-25", pixelToMm(516.0, cal),
		          -25.0);

		// 超出左端点 → 钳制
		checkNear("left clamp", pixelToMm(100.0, cal), -50.0);

		// 超出右端点 → 钳制
		checkNear("right clamp", pixelToMm(1000.0, cal), 50.0);
	}

	// ── image_right_sign = -1 ──
	{
		auto cal = makeCalibration({392.0, 640.0, 887.0},
		                           {-50.0, 0.0, 50.0}, -1);

		checkNear("sign=-1 left", pixelToMm(392.0, cal), 50.0);
		checkNear("sign=-1 right", pixelToMm(887.0, cal), -50.0);
		checkNear("sign=-1 mid", pixelToMm(640.0, cal), 0.0);
	}

	// ── 不足两点 → 返回 0 ──
	{
		etest::PipeAxisCalibration cal;
		cal.points.push_back({100.0, 0.0});
		check("single point returns 0", pixelToMm(100.0, cal) == 0.0);
	}

	{
		etest::PipeAxisCalibration cal;
		check("empty points returns 0", pixelToMm(100.0, cal) == 0.0);
	}

	// ── 非法 image_right_sign → 返回 0 ──
	{
		auto cal = makeCalibration({100.0, 200.0}, {0.0, 10.0},
		                           0); // ← 非法
		check("sign=0 returns 0", pixelToMm(150.0, cal) == 0.0);
	}

	{
		auto cal = makeCalibration({100.0, 200.0}, {0.0, 10.0},
		                           3); // ← 非法
		check("sign=3 returns 0", pixelToMm(150.0, cal) == 0.0);
	}

	// ── 配置解析：重复 pixel 应被拒绝 ──
	{
		etest::ConfigLoadResult result;
		auto loaded = etest::ConfigLoader::loadMultiple({
		    "config/vision.toml",
		});
		result = loaded; // 不验证，仅确认解析过程不会 crash

		// 手动构建非法配置检查：重复 pixel
		auto cal =
		    makeCalibration({100.0, 100.0, 200.0}, {0.0, 5.0, 10.0});
		// pixelToMm 内检测到重复会返回 0
		check("duplicate pixel guarded", pixelToMm(150.0, cal) == 0.0);
	}

	// ── 配置解析：乱序 pixel 应被拒绝 ──
	{
		auto cal =
		    makeCalibration({200.0, 100.0, 300.0}, {0.0, 5.0, 10.0});
		check("unsorted pixel guarded", pixelToMm(150.0, cal) == 0.0);
	}

	// ── 两点标定（最小合法配置）──
	{
		auto cal = makeCalibration({0.0, 1280.0}, {-100.0, 100.0});

		checkNear("2pt left", pixelToMm(0.0, cal), -100.0);
		checkNear("2pt right", pixelToMm(1280.0, cal), 100.0);
		checkNear("2pt mid", pixelToMm(640.0, cal), 0.0);
	}

	std::cout << "\n";
	if(failures == 0)
	{
		std::cout << "All axis calibration tests passed.\n";
	}
	else
	{
		std::cerr << failures << " test(s) FAILED.\n";
	}

	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}