#include "vision/vision.hpp"
#include "core/logger.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <opencv2/imgproc.hpp>

namespace etest::vision
{

	VisionProcessor::VisionProcessor(VisionConfig config):
	config_(std::move(config))
	{
	}

	VisionResult VisionProcessor::process(const cv::Mat& frame,
	                                      VisionMode mode) noexcept
	{
		++frame_id_counter_;
		auto now = std::chrono::steady_clock::now();
		auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
		              now.time_since_epoch())
		              .count();
		if(frame.empty())
		{
			if(!empty_frame_reported_)
			{
				ETEST_LOG_ERROR("VISION", "empty frame");
				empty_frame_reported_ = true;
			}
			VisionResult r;
			r.frame_id = frame_id_counter_;
			r.timestamp_ms = static_cast<int64_t>(ts);
			r.target_type = (mode == VisionMode::Ball) ? "BALL" : "";
			r.error_code = "EMPTY_FRAME";
			return r;
		}
		if(empty_frame_reported_)
		{
			ETEST_LOG_INFO("VISION", "frame recovered");
			empty_frame_reported_ = false;
		}
		VisionResult r;
		r.frame_id = frame_id_counter_;
		r.timestamp_ms = static_cast<int64_t>(ts);
		if(mode == VisionMode::Ball)
			r.target_type = "BALL";
		try
		{
			switch(mode)
			{
			case VisionMode::ColorTarget:
				r = detectColorTarget(frame);
				r.frame_id = frame_id_counter_;
				r.timestamp_ms = static_cast<int64_t>(ts);
				return r;
			case VisionMode::Ball:
				r = detectBall(frame);
				r.frame_id = frame_id_counter_;
				r.timestamp_ms = static_cast<int64_t>(ts);
				return r;
			default:
				r.error_code = "UNSUPPORTED_MODE";
				return r;
			}
		}
		catch(const cv::Exception& e)
		{
			ETEST_LOG_ERROR("VISION", std::string("cv:") + e.what());
			r.valid = false;
			r.error_code = "CV_EXCEPTION";
			return r;
		}
		catch(const std::exception& e)
		{
			ETEST_LOG_ERROR("VISION", std::string("ex:") + e.what());
			r.valid = false;
			r.error_code = "STD_EXCEPTION";
			return r;
		}
		catch(...)
		{
			ETEST_LOG_ERROR("VISION", "unknown");
			r.valid = false;
			r.error_code = "UNKNOWN_EXCEPTION";
			return r;
		}
	}

	VisionResult VisionProcessor::detectColorTarget(
	    const cv::Mat& frame)
	{
		VisionResult r;
		r.target_type = "RED_TARGET";
		cv::Mat hsv;
		cv::cvtColor(frame, hsv, cv::COLOR_BGR2HSV);
		cv::Mat m1, m2;
		cv::inRange(
		    hsv,
		    cv::Scalar(config_.red_h1_min, config_.saturation_min,
		               config_.value_min),
		    cv::Scalar(config_.red_h1_max, 255, 255), m1);
		cv::inRange(
		    hsv,
		    cv::Scalar(config_.red_h2_min, config_.saturation_min,
		               config_.value_min),
		    cv::Scalar(config_.red_h2_max, 255, 255), m2);
		cv::Mat mask = m1 | m2;
		auto k = cv::getStructuringElement(
		    cv::MORPH_ELLIPSE,
		    cv::Size(config_.morphology_kernel,
		             config_.morphology_kernel));
		cv::morphologyEx(mask, mask, cv::MORPH_OPEN, k);
		cv::morphologyEx(mask, mask, cv::MORPH_CLOSE, k);
		std::vector<std::vector<cv::Point>> cs;
		cv::findContours(mask, cs, cv::RETR_EXTERNAL,
		                 cv::CHAIN_APPROX_SIMPLE);
		if(cs.empty())
		{
			r.error_code = "NO_CONTOUR";
			return r;
		}
		auto mx = std::max_element(
		    cs.begin(), cs.end(), [](auto& l, auto& r) {
			    return cv::contourArea(l) < cv::contourArea(r);
		    });
		double a = cv::contourArea(*mx);
		if(a < config_.min_area)
		{
			r.error_code = "AREA_TOO_SMALL";
			return r;
		}
		auto m = cv::moments(*mx);
		if(m.m00 == 0.0)
		{
			r.error_code = "ZERO_MOMENT";
			return r;
		}
		r.valid = true;
		r.x = m.m10 / m.m00;
		r.y = m.m01 / m.m00;
		r.confidence = 1.0;
		auto rect = cv::minAreaRect(*mx);
		r.angle = rect.angle;
		r.distance = std::sqrt(a);
		return r;
	}

	TrackResult VisionProcessor::detectBrownPipe(
	    const cv::Mat& frame) noexcept
	{
		TrackResult r;
		try
		{
			if(frame.empty())
			{
				ETEST_LOG_ERROR("PIPE", "empty");
				return r;
			}
			auto& ball = config_.ball;
			cv::Rect roi(ball.pipe_search_roi_x, ball.pipe_search_roi_y,
			             ball.pipe_search_roi_w,
			             ball.pipe_search_roi_h);
			roi &= cv::Rect(0, 0, frame.cols, frame.rows);
			if(roi.empty() || roi.area() < 100)
			{
				debug_track_mask_ =
				    cv::Mat::zeros(frame.size(), CV_8UC1);
				return r;
			}
			double ra = static_cast<double>(roi.area());
			cv::Mat si = frame(roi), hsv;
			cv::cvtColor(si, hsv, cv::COLOR_BGR2HSV);
			cv::Mat bm;
			cv::inRange(hsv,
			            cv::Scalar(ball.brown_h_min, ball.brown_s_min,
			                       ball.brown_v_min),
			            cv::Scalar(ball.brown_h_max, ball.brown_s_max,
			                       ball.brown_v_max),
			            bm);
			auto ck = cv::getStructuringElement(
			    cv::MORPH_RECT,
			    cv::Size(ball.pipe_close_kernel_w,
			             ball.pipe_close_kernel_h));
			cv::morphologyEx(bm, bm, cv::MORPH_CLOSE, ck);
			auto ok = cv::getStructuringElement(
			    cv::MORPH_ELLIPSE,
			    cv::Size(ball.pipe_open_kernel, ball.pipe_open_kernel));
			cv::morphologyEx(bm, bm, cv::MORPH_OPEN, ok);
			debug_track_mask_ = cv::Mat::zeros(frame.size(), CV_8UC1);
			bm.copyTo(debug_track_mask_(roi));
			std::vector<std::vector<cv::Point>> cs;
			cv::findContours(bm, cs, cv::RETR_EXTERNAL,
			                 cv::CHAIN_APPROX_SIMPLE);
			int tc = static_cast<int>(cs.size()), rj_a = 0, rj_as = 0,
			    rj_an = 0, rj_f = 0, rj_ss = 0;
			double bs = -1.0;
			cv::RotatedRect br;
			double ba = 0, bas = 0, ban = 0, bf = 0, bss = 0;
			for(auto& c: cs)
			{
				double a = cv::contourArea(c);
				if(a < ra * ball.pipe_min_area_ratio)
				{
					++rj_a;
					continue;
				}
				auto rect = cv::minAreaRect(c);
				double ls = std::max(rect.size.width, rect.size.height),
				       ss = std::min(rect.size.width, rect.size.height);
				if(ss < ball.pipe_min_short_side_px)
				{
					++rj_ss;
					continue;
				}
				double as = ls / ss;
				if(as < ball.pipe_min_aspect_ratio)
				{
					++rj_as;
					continue;
				}
				cv::Point2f pts[4];
				rect.points(pts);
				auto e1 = pts[1] - pts[0], e2 = pts[2] - pts[1];
				auto le = cv::norm(e1) >= cv::norm(e2) ? e1 : e2;
				double ang = std::atan2(le.y, le.x) * 180.0 / CV_PI;
				double hd = std::min(std::abs(ang),
				                     std::abs(180.0 - std::abs(ang)));
				if(hd > ball.pipe_horizontal_angle_max)
				{
					++rj_an;
					continue;
				}
				double ra2 = rect.size.width * rect.size.height,
				       fl = (ra2 > 0) ? a / ra2 : 0;
				if(fl < ball.pipe_min_fill_ratio)
				{
					++rj_f;
					continue;
				}
				double sc = a * std::min(as, 20.0) * fl;
				if(sc > bs)
				{
					bs = sc;
					br = rect;
					ba = a;
					bas = as;
					ban = ang;
					bf = fl;
					bss = ss;
				}
			}
			{
				static auto lt = std::chrono::steady_clock::now();
				auto n = std::chrono::steady_clock::now();
				if(std::chrono::duration_cast<
				       std::chrono::milliseconds>(n - lt)
				       .count()
				   >= 1000)
				{
					ETEST_LOG_INFO(
					    "PIPE_DEBUG",
					    "roi=" + std::to_string(roi.x) + ","
					        + std::to_string(roi.y) + " "
					        + std::to_string(roi.width) + "x"
					        + std::to_string(roi.height) + " nz="
					        + std::to_string(cv::countNonZero(bm))
					        + " c=" + std::to_string(tc)
					        + " rj(a=" + std::to_string(rj_a)
					        + " as=" + std::to_string(rj_as)
					        + " an=" + std::to_string(rj_an)
					        + " f=" + std::to_string(rj_f)
					        + " ss=" + std::to_string(rj_ss)
					        + ") best(a=" + std::to_string(ba)
					        + " as=" + std::to_string(bas)
					        + " an=" + std::to_string(ban)
					        + " f=" + std::to_string(bf)
					        + " ss=" + std::to_string(bss) + ") st="
					        + std::to_string(track_stable_count_));
					lt = n;
				}
			}
			if(bs < 0.0)
				return r;
			br.center.x += roi.x;
			br.center.y += roi.y;
			r.valid = true;
			r.rect = br;
			r.bounding_roi = cv::Rect(
			    static_cast<int>(br.center.x - br.size.width / 2),
			    static_cast<int>(br.center.y - br.size.height / 2),
			    static_cast<int>(br.size.width),
			    static_cast<int>(br.size.height));
			r.bounding_roi &= cv::Rect(0, 0, frame.cols, frame.rows);
			r.confidence = std::clamp(bs / (ra * 20.0), 0.0, 1.0);
			cv::Point2f pts[4];
			br.points(pts);
			double e01 = cv::norm(pts[1] - pts[0]),
			       e12 = cv::norm(pts[2] - pts[1]);
			if(e01 >= e12)
			{
				r.axis_p1 = (pts[0] + pts[3]) * 0.5F;
				r.axis_p2 = (pts[1] + pts[2]) * 0.5F;
			}
			else
			{
				r.axis_p1 = (pts[0] + pts[1]) * 0.5F;
				r.axis_p2 = (pts[2] + pts[3]) * 0.5F;
			}
			return r;
		}
		catch(const cv::Exception& e)
		{
			ETEST_LOG_ERROR("PIPE", std::string("cv:") + e.what());
		}
		catch(...)
		{
			ETEST_LOG_ERROR("PIPE", "unknown");
		}
		return r;
	}

	bool VisionProcessor::isTrackSimilar(
	    const TrackResult& a, const TrackResult& b) const noexcept
	{
		if(!a.valid || !b.valid)
			return false;
		auto& ball = config_.ball;
		return cv::norm(a.rect.center - b.rect.center)
		    < ball.pipe_similarity_center_max_px
		    && std::abs(
		           std::max(a.rect.size.width, a.rect.size.height)
		           - std::max(b.rect.size.width, b.rect.size.height))
		    < ball.pipe_similarity_length_max_px;
	}

	cv::Rect VisionProcessor::makeInnerRoi(const cv::Rect& tr,
	                                       const cv::Size& ws) noexcept
	{
		int mx = std::max(1, static_cast<int>(tr.width * 0.03)),
		    my = std::max(1, static_cast<int>(tr.height * 0.15));
		cv::Rect in(tr.x + mx, tr.y + my,
		            std::max(10, tr.width - mx * 2),
		            std::max(10, tr.height - my * 2));
		in &= cv::Rect(0, 0, ws.width, ws.height);
		if(in.width < 10 || in.height < 10)
			return cv::Rect();
		return in;
	}

	bool VisionProcessor::orderTrackCorners(
	    const cv::RotatedRect& rect,
	    std::array<cv::Point2f, 4>& ordered) noexcept
	{
		try
		{
			cv::Point2f pts[4];
			rect.points(pts);
			std::sort(pts, pts + 4, [](auto& a, auto& b) {
				return (a.x + a.y) < (b.x + b.y);
			});
			auto tl = (pts[0].x <= pts[1].x) ? pts[0] : pts[1],
			     tr = (pts[0].x <= pts[1].x) ? pts[1] : pts[0];
			auto bl = (pts[2].x <= pts[3].x) ? pts[2] : pts[3],
			     br = (pts[2].x <= pts[3].x) ? pts[3] : pts[2];
			double a = std::abs((tr.x - tl.x) * (br.y - tl.y)
			                    - (tr.y - tl.y) * (br.x - tl.x));
			if(a < 100.0)
			{
				auto n = std::chrono::steady_clock::now();
				if(std::chrono::duration_cast<
				       std::chrono::milliseconds>(
				       n - last_corner_order_error_time_)
				       .count()
				   >= 1000)
				{
					ETEST_LOG_ERROR("PIPE", "degenerate quad");
					last_corner_order_error_time_ = n;
				}
				return false;
			}
			double tl2 = cv::norm(tr - tl), bl2 = cv::norm(br - bl),
			       ll2 = cv::norm(bl - tl), rl2 = cv::norm(br - tr);
			if(tl2 < 1.0 || bl2 < 1.0 || ll2 < 1.0 || rl2 < 1.0)
			{
				auto n = std::chrono::steady_clock::now();
				if(std::chrono::duration_cast<
				       std::chrono::milliseconds>(
				       n - last_corner_order_error_time_)
				       .count()
				   >= 1000)
				{
					ETEST_LOG_ERROR("PIPE", "edge too short");
					last_corner_order_error_time_ = n;
				}
				return false;
			}
			if((tl.x + bl.x) > (tr.x + br.x))
			{
				std::swap(tl, tr);
				std::swap(bl, br);
			}
			ordered[0] = tl;
			ordered[1] = tr;
			ordered[2] = br;
			ordered[3] = bl;
			return true;
		}
		catch(const cv::Exception& e)
		{
			auto n = std::chrono::steady_clock::now();
			if(std::chrono::duration_cast<std::chrono::milliseconds>(
			       n - last_corner_order_error_time_)
			       .count()
			   >= 1000)
			{
				ETEST_LOG_ERROR("PIPE", std::string("cv:") + e.what());
				last_corner_order_error_time_ = n;
			}
		}
		catch(...)
		{
			auto n = std::chrono::steady_clock::now();
			if(std::chrono::duration_cast<std::chrono::milliseconds>(
			       n - last_corner_order_error_time_)
			       .count()
			   >= 1000)
			{
				ETEST_LOG_ERROR("PIPE", "unknown");
				last_corner_order_error_time_ = n;
			}
		}
		return false;
	}

	bool VisionProcessor::updateWarpMatrices() noexcept
	{
		try
		{
			if(!locked_pipe_points_valid_)
				return false;
			auto& ball = config_.ball;
			int ww = ball.pipe_warp_width, wh = ball.pipe_warp_height;
			const cv::Point2f dst[4] = {
			    {0, 0},
			    {static_cast<float>(ww - 1), 0},
			    {static_cast<float>(ww - 1),
			     static_cast<float>(wh - 1)},
			    {0, static_cast<float>(wh - 1)}};
			auto nw = cv::getPerspectiveTransform(
			         locked_pipe_points_.data(), dst),
			     ni = cv::getPerspectiveTransform(
			         dst, locked_pipe_points_.data());
			if(nw.empty() || ni.empty())
				goto fail;
			for(int r = 0; r < 3; ++r)
				for(int c = 0; c < 3; ++c)
					if(!std::isfinite(nw.at<double>(r, c))
					   || !std::isfinite(ni.at<double>(r, c)))
						goto fail;
			if(std::abs(cv::determinant(nw)) < 1e-8)
				goto fail;
			warp_matrix_ = nw;
			inverse_warp_matrix_ = ni;
			return true;
fail: {
	auto n = std::chrono::steady_clock::now();
	if(std::chrono::duration_cast<std::chrono::milliseconds>(
	       n - last_warp_update_error_time_)
	       .count()
	   >= 1000)
	{
		ETEST_LOG_ERROR("PIPE", "updateWarp failed");
		last_warp_update_error_time_ = n;
	}
}
			return false;
		}
		catch(const cv::Exception& e)
		{
			auto n = std::chrono::steady_clock::now();
			if(std::chrono::duration_cast<std::chrono::milliseconds>(
			       n - last_warp_update_error_time_)
			       .count()
			   >= 1000)
			{
				ETEST_LOG_ERROR("PIPE",
				                std::string("warp:") + e.what());
				last_warp_update_error_time_ = n;
			}
		}
		catch(...)
		{
			auto n = std::chrono::steady_clock::now();
			if(std::chrono::duration_cast<std::chrono::milliseconds>(
			       n - last_warp_update_error_time_)
			       .count()
			   >= 1000)
			{
				ETEST_LOG_ERROR("PIPE", "warp unknown");
				last_warp_update_error_time_ = n;
			}
		}
		return false;
	}

	// ── detectBallCandidates with reject_code ──
	std::vector<BallCandidate> VisionProcessor::detectBallCandidates(
	    const cv::Mat& warped_roi) noexcept
	{
		std::vector<BallCandidate> candidates;
		try
		{
			if(warped_roi.empty() || warped_roi.cols < 10
			   || warped_roi.rows < 10)
				return candidates;
			auto& ball = config_.ball;
			// gray_raw for mean_inner/ring
			cv::Mat gray_raw;
			if(warped_roi.channels() == 3)
				cv::cvtColor(warped_roi, gray_raw, cv::COLOR_BGR2GRAY);
			else
				gray_raw = warped_roi.clone();
			// gray_blurred for HoughCircles
			cv::Mat gray_blurred;
			cv::GaussianBlur(gray_raw, gray_blurred, cv::Size(7, 7),
			                 1.5);
			std::vector<cv::Vec3f> circles;
			cv::HoughCircles(gray_blurred, circles, cv::HOUGH_GRADIENT,
			                 ball.hough_dp, ball.hough_min_distance,
			                 ball.hough_param1, ball.hough_param2,
			                 ball.ball_min_radius,
			                 ball.ball_max_radius);
			if(circles.empty())
				return candidates;
			double rw = static_cast<double>(warped_roi.cols),
			       rh = static_cast<double>(warped_roi.rows);
			double min_y = rh * ball.ball_min_center_y_ratio,
			       max_y = rh * ball.ball_max_center_y_ratio,
			       exp_y = rh * ball.ball_expected_center_y_ratio,
			       exp_r = ball.ball_expected_radius;
			double denom_cs = ball.ball_good_ring_contrast
			    - ball.ball_min_ring_contrast;
			if(denom_cs < 1e-6)
				denom_cs = 1.0;
			for(auto& c: circles)
			{
				float cx = c[0], cy = c[1], cr = c[2];
				BallCandidate bc;
				bc.center = cv::Point2f(cx, cy);
				bc.radius = cr;
				bc.normalized_x = cx / (rw - 1.0);
				// reject code checks
				if(cx - cr < 0 || cx + cr >= rw || cy - cr < 0
				   || cy + cr >= rh)
				{
					bc.reject_code = 1;
					candidates.push_back(bc);
					continue;
				}
				if(cy < min_y || cy > max_y)
				{
					bc.reject_code = 2;
					candidates.push_back(bc);
					continue;
				}
				if(cr < static_cast<float>(ball.ball_min_radius)
				   || cr > static_cast<float>(ball.ball_max_radius))
				{
					bc.reject_code = 3;
					candidates.push_back(bc);
					continue;
				}
				// outer_radius = ceil(radius*1.30)
				int outer_r = static_cast<int>(std::ceil(cr * 1.30f));
				int bx = std::max(0, static_cast<int>(cx - outer_r)),
				    by = std::max(0, static_cast<int>(cy - outer_r));
				int bw = std::min(static_cast<int>(cx + outer_r),
				                  warped_roi.cols)
				    - bx,
				    bh = std::min(static_cast<int>(cy + outer_r),
				                  warped_roi.rows)
				    - by;
				if(bw <= 0 || bh <= 0)
				{
					bc.reject_code = 1;
					candidates.push_back(bc);
					continue;
				}
				cv::Rect bbox(bx, by, bw, bh);
				cv::Mat local_gray = gray_raw(bbox);
				cv::Mat inner_mask =
				    cv::Mat::zeros(bbox.size(), CV_8UC1);
				cv::circle(inner_mask,
				           cv::Point(static_cast<int>(cx - bx),
				                     static_cast<int>(cy - by)),
				           static_cast<int>(cr * 0.70f), 255, -1);
				if(cv::countNonZero(inner_mask) < 20)
				{
					bc.reject_code = 7;
					candidates.push_back(bc);
					continue;
				}
				double mean_inner = cv::mean(local_gray, inner_mask)[0];
				cv::Mat ring_mask =
				    cv::Mat::zeros(bbox.size(), CV_8UC1);
				cv::circle(ring_mask,
				           cv::Point(static_cast<int>(cx - bx),
				                     static_cast<int>(cy - by)),
				           outer_r, 255, -1);
				cv::Mat inner_ring =
				    cv::Mat::zeros(bbox.size(), CV_8UC1);
				cv::circle(inner_ring,
				           cv::Point(static_cast<int>(cx - bx),
				                     static_cast<int>(cy - by)),
				           static_cast<int>(cr * 0.95f), 255, -1);
				cv::Mat ring_only = ring_mask - inner_ring;
				if(cv::countNonZero(ring_only) < 20)
				{
					bc.reject_code = 7;
					candidates.push_back(bc);
					continue;
				}
				double mean_ring = cv::mean(local_gray, ring_only)[0],
				       ring_contrast = mean_ring - mean_inner;
				bc.mean_inner_gray = mean_inner;
				bc.mean_ring_gray = mean_ring;
				bc.ring_contrast = ring_contrast;
				if(mean_inner > ball.ball_max_inner_gray)
				{
					bc.reject_code = 4;
					candidates.push_back(bc);
					continue;
				}
				if(ring_contrast < ball.ball_min_ring_contrast)
				{
					bc.reject_code = 5;
					candidates.push_back(bc);
					continue;
				}
				double rs = 1.0
				    - std::clamp(std::abs(cr - exp_r) / exp_r, 0.0,
				                 1.0);
				double cs = 1.0
				    - std::clamp(std::abs(cy - exp_y) / (rh * 0.5), 0.0,
				                 1.0);
				double ds =
				    1.0 - std::clamp(mean_inner / 255.0, 0.0, 1.0);
				double cts = std::clamp(
				    (ring_contrast - ball.ball_min_ring_contrast)
				        / denom_cs,
				    0.0, 1.0);
				double quality =
				    0.30 * rs + 0.25 * cs + 0.25 * ds + 0.20 * cts;
				bc.radius_score = rs;
				bc.center_score = cs;
				bc.darkness_score = ds;
				bc.contrast_score = cts;
				bc.quality = quality;
				if(quality < ball.ball_min_quality)
				{
					bc.reject_code = 6;
					candidates.push_back(bc);
					continue;
				}
				bc.reject_code = 0;
				bc.passed = true;
				candidates.push_back(bc);
			}
			std::sort(candidates.begin(), candidates.end(),
			          [](auto& a, auto& b) {
				          return a.quality > b.quality;
			          });
		}
		catch(const cv::Exception& e)
		{
			ETEST_LOG_ERROR(
			    "BALL", std::string("detectCandidates:") + e.what());
		}
		catch(...)
		{
			ETEST_LOG_ERROR("BALL", "detectCandidates unknown");
		}
		return candidates;
	}

	// ── detectBall with ratio zero, tracker-based axis, 3-class error, acquire confirm ──
	VisionResult VisionProcessor::detectBall(const cv::Mat& frame)
	{
		VisionResult r;
		r.target_type = "BALL";
		auto& ball = config_.ball;
		int kW = ball.work_width, kH = ball.work_height;
		// letterbox
		cv::Mat wf;
		double sc = std::min(static_cast<double>(kW) / frame.cols,
		                     static_cast<double>(kH) / frame.rows);
		int sw = static_cast<int>(frame.cols * sc),
		    sh = static_cast<int>(frame.rows * sc);
		cv::Mat scaled;
		cv::resize(frame, scaled, cv::Size(sw, sh), 0, 0,
		           cv::INTER_LINEAR);
		wf = cv::Mat(kH, kW, frame.type(), cv::Scalar(0, 0, 0));
		int ox = (kW - sw) / 2, oy = (kH - sh) / 2;
		scaled.copyTo(wf(cv::Rect(ox, oy, sw, sh)));
		// fixed mode
		if(ball.pipe_mode == "fixed")
		{
			const cv::Point2f src[4] = {
			    {static_cast<float>(ball.pipe_fixed_tl_x),
			     static_cast<float>(ball.pipe_fixed_tl_y)},
			    {static_cast<float>(ball.pipe_fixed_tr_x),
			     static_cast<float>(ball.pipe_fixed_tr_y)},
			    {static_cast<float>(ball.pipe_fixed_br_x),
			     static_cast<float>(ball.pipe_fixed_br_y)},
			    {static_cast<float>(ball.pipe_fixed_bl_x),
			     static_cast<float>(ball.pipe_fixed_bl_y)}};
			double a = std::abs(
			    (src[1].x - src[0].x) * (src[3].y - src[0].y)
			    - (src[1].y - src[0].y) * (src[3].x - src[0].x));
			if(a < 100.0)
			{
				r.error_code = "TRACK_CONFIG_INVALID";
				return r;
			}
			if(!warp_locked_)
			{
				int ww = ball.pipe_warp_width,
				    wh = ball.pipe_warp_height;
				const cv::Point2f dst[4] = {
				    {0, 0},
				    {static_cast<float>(ww - 1), 0},
				    {static_cast<float>(ww - 1),
				     static_cast<float>(wh - 1)},
				    {0, static_cast<float>(wh - 1)}};
				warp_matrix_ = cv::getPerspectiveTransform(src, dst);
				if(warp_matrix_.empty()
				   || std::isnan(warp_matrix_.at<double>(0, 0)))
				{
					r.error_code = "PIPE_WARP_FAILED";
					return r;
				}
				warp_locked_ = true;
				track_locked_ = true;
				locked_track_.valid = true;
				locked_track_.bounding_roi = cv::Rect(0, 0, kW, kH);
				locked_track_.axis_p1 = src[0];
				locked_track_.axis_p2 = src[1];
			}
			goto warp_and_detect;
		}
		// auto pipe
		{
			auto track = detectBrownPipe(wf);
			if(!track_locked_)
			{
				if(track.valid)
				{
					if(isTrackSimilar(track, locked_track_))
					{
						++track_stable_count_;
						if(track_stable_count_
						   >= ball.pipe_stable_frames)
						{
							std::array<cv::Point2f, 4> ord;
							if(orderTrackCorners(track.rect, ord))
							{
								locked_pipe_points_ = ord;
								locked_pipe_points_valid_ = true;
								if(updateWarpMatrices())
								{
									track_locked_ = true;
									locked_track_ = track;
									warp_locked_ = true;
									track_stable_count_ = 0;
									track_lost_frame_count_ = 0;
									ETEST_LOG_INFO("VISION",
									               "pipe locked");
								}
							}
						}
					}
					else
					{
						track_stable_count_ = 1;
						locked_track_ = track;
					}
				}
				else
				{
					track_stable_count_ = 0;
				}
			}
			else
			{
				if(track.valid && isTrackSimilar(track, locked_track_))
				{
					if(track_lost_frame_count_ > 0)
					{
						auto n = std::chrono::steady_clock::now();
						if(std::chrono::duration_cast<
						       std::chrono::milliseconds>(
						       n - last_pipe_recovered_info_time_)
						       .count()
						   >= 1000)
						{
							ETEST_LOG_INFO("VISION", "pipe recovered");
							last_pipe_recovered_info_time_ = n;
						}
						track_lost_frame_count_ = 0;
					}
					locked_track_ = track;
					std::array<cv::Point2f, 4> dp;
					if(orderTrackCorners(track.rect, dp))
					{
						if(ball.pipe_update_each_frame)
						{
							double al = std::clamp(
							    ball.pipe_geometry_alpha, 0.01, 1.0);
							for(int i = 0; i < 4; ++i)
							{
								locked_pipe_points_[i].x =
								    static_cast<float>(
								        al * dp[i].x
								        + (1.0 - al)
								            * locked_pipe_points_[i].x);
								locked_pipe_points_[i].y =
								    static_cast<float>(
								        al * dp[i].y
								        + (1.0 - al)
								            * locked_pipe_points_[i].y);
							}
						}
						else
							locked_pipe_points_ = dp;
						updateWarpMatrices();
					}
				}
				else
				{
					if(track_lost_frame_count_ == 0)
					{
						auto n = std::chrono::steady_clock::now();
						if(std::chrono::duration_cast<
						       std::chrono::milliseconds>(
						       n - last_pipe_lost_warn_time_)
						       .count()
						   >= 1000)
						{
							ETEST_LOG_WARN("VISION", "pipe lost");
							last_pipe_lost_warn_time_ = n;
						}
					}
					++track_lost_frame_count_;
					if(track_lost_frame_count_
					   >= ball.pipe_lost_timeout_frames)
					{
						track_locked_ = false;
						track_stable_count_ = 0;
						track_lost_frame_count_ = 0;
						warp_locked_ = false;
						locked_pipe_points_valid_ = false;
						ball_state_ = BallState::FIND_TRACK;
						zero_locked_ = false;
						zero_buffer_.clear();
						tracker_initialized_ = false;
						tracker_lost_frames_ = 0;
						reacquire_confirm_count_ = 0;
						acquire_confirm_count_ = 0;
					}
				}
			}
		}
		if(!track_locked_)
		{
			r.error_code = "PIPE_NOT_STABLE";
			return r;
		}

warp_and_detect:
		cv::Mat warped;
		cv::warpPerspective(
		    wf, warped, warp_matrix_,
		    cv::Size(ball.pipe_warp_width, ball.pipe_warp_height),
		    cv::INTER_LINEAR);
		debug_warped_pipe_ = warped.clone();
		int mx = static_cast<int>(ball.pipe_warp_width
		                          * ball.pipe_inner_margin_x_ratio),
		    my = static_cast<int>(ball.pipe_warp_height
		                          * ball.pipe_inner_margin_y_ratio);
		cv::Rect inner_roi(mx, my,
		                   std::max(1, ball.pipe_warp_width - 2 * mx),
		                   std::max(1, ball.pipe_warp_height - 2 * my));
		inner_roi &= cv::Rect(0, 0, warped.cols, warped.rows);
		if(inner_roi.empty() || inner_roi.width < 10
		   || inner_roi.height < 5)
		{
			r.error_code = "PIPE_ROI_INVALID";
			return r;
		}
		cv::Mat roi_img = warped(inner_roi);
		// Canny
		{
			cv::Mat g;
			if(roi_img.channels() == 3)
				cv::cvtColor(roi_img, g, cv::COLOR_BGR2GRAY);
			else
				g = roi_img.clone();
			cv::Canny(g, debug_ball_binary_, 50, 150);
		}
		auto candidates = detectBallCandidates(roi_img);
		// 候选坐标从 ROI 局部空间转换到 warped 全图空间
		for(auto& c: candidates)
		{
			c.center.x += inner_roi.x;
			c.center.y += inner_roi.y;
			c.normalized_x = c.center.x
			    / std::max(1.0, static_cast<double>(warped.cols) - 1.0);
		}
		debug_ball_candidates_ = candidates;
		auto now = std::chrono::steady_clock::now();

		// threshold logs
		auto throt = [this](const std::string& code,
		                    const std::string& tag,
		                    const std::string& msg) {
			auto n = std::chrono::steady_clock::now();
			auto el =
			    std::chrono::duration_cast<std::chrono::milliseconds>(
			        n - last_ball_error_time_);
			if(code != last_ball_error_code_)
			{
				ETEST_LOG_WARN(tag.c_str(), msg.c_str());
				last_ball_error_code_ = code;
				last_ball_error_time_ = n;
			}
			else if(el.count() >= 2000)
			{
				ETEST_LOG_WARN(tag.c_str(), msg.c_str());
				last_ball_error_time_ = n;
			}
		};

		// (1) NO_HOUGH_CIRCLE
		if(candidates.empty())
		{
			if(tracker_initialized_)
			{
				++tracker_lost_frames_;
				++ball_lost_frame_count_;
				if(!ball_lost_)
				{
					ball_lost_ = true;
				}
			}
			r.error_code = "NO_HOUGH_CIRCLE";
			throt("NO_HOUGH_CIRCLE", "BALL", "no Hough circle");
			return r;
		}

		// (2) HOUGH_CANDIDATES_FILTERED
		bool any_passed = false;
		for(auto& c: candidates)
			if(c.reject_code == 0)
			{
				any_passed = true;
				break;
			}
		if(!any_passed)
		{
			if(tracker_initialized_)
			{
				++tracker_lost_frames_;
			}
			r.error_code = "HOUGH_CANDIDATES_FILTERED";
			throt("HOUGH_CANDIDATES_FILTERED", "BALL",
			      "all Hough candidates filtered");
			return r;
		}

		// prediction
		double dt = 0.02;
		if(tracker_initialized_)
		{
			dt = std::chrono::duration<double>(now - tracker_last_time_)
			         .count();
			dt = std::clamp(dt, 0.005, 0.100);
			predicted_position_ratio_ = tracked_position_ratio_
			    + tracked_velocity_ratio_per_s_ * dt;
			predicted_position_ratio_ =
			    std::clamp(predicted_position_ratio_, 0.0, 1.0);
		}
		tracker_last_time_ = now;
		bool in_global_reacquire =
		    (tracker_lost_frames_
		     >= ball.tracker_global_reacquire_frames);

		// gate
		double gate = ball.tracker_gate_ratio
		    * (1.0
		       + ball.tracker_gate_growth_per_lost_frame
		           * tracker_lost_frames_);
		gate = std::min(gate, ball.tracker_max_gate_ratio);

		const BallCandidate* best = nullptr;
		double best_score = -1e9, best_gate_error = 0.0;
		int in_gate_count = 0;
		for(auto& c: candidates)
		{
			if(c.reject_code != 0)
			{
				c.association_rejected = true;
				continue;
			}
			double delta =
			    std::abs(c.normalized_x - predicted_position_ratio_);
			if(!in_global_reacquire && tracker_initialized_)
			{
				if(delta > gate)
				{
					c.association_rejected = true;
					continue;
				}
				++in_gate_count;
				double score = c.quality - 2.0 * delta / gate;
				if(score > best_score)
				{
					best_score = score;
					best = &c;
					best_gate_error = delta;
				}
			}
			else
			{
				// no tracker or global reacquire → use quality directly
				++in_gate_count;
				if(c.quality > best_score)
				{
					best_score = c.quality;
					best = &c;
					best_gate_error = delta;
				}
			}
		}

		// (3) CANDIDATES_OUT_OF_GATE
		if(best == nullptr)
		{
			if(tracker_initialized_)
			{
				++tracker_lost_frames_;
				++ball_lost_frame_count_;
			}
			r.error_code = "CANDIDATES_OUT_OF_GATE";
			throt("CANDIDATES_OUT_OF_GATE", "BALL",
			      "candidates out of gate");
			return r;
		}

		// acquire confirm
		if(!tracker_initialized_ || in_global_reacquire)
		{
			if(acquire_confirm_count_ == 0)
			{
				acquire_candidate_ratio_ = best->normalized_x;
				acquire_confirm_count_ = 1;
				r.error_code = "BALL_LOST";
				return r;
			}
			if(std::abs(best->normalized_x - acquire_candidate_ratio_)
			   < ball.tracker_gate_ratio)
			{
				++acquire_confirm_count_;
				acquire_candidate_ratio_ = best->normalized_x;
			}
			else
			{
				acquire_confirm_count_ = 1;
				acquire_candidate_ratio_ = best->normalized_x;
			}
			if(acquire_confirm_count_ < ball.acquire_confirm_frames)
			{
				r.error_code = "BALL_LOST";
				return r;
			}
			acquire_confirm_count_ = 0;
			tracker_lost_frames_ = 0;
		}

		// tracker update
		double measurement = best->normalized_x;
		if(!tracker_initialized_)
		{
			tracked_position_ratio_ = measurement;
			tracked_velocity_ratio_per_s_ = 0.0;
			predicted_position_ratio_ = measurement;
			tracker_initialized_ = true;
		}
		else
		{
			double residual = measurement - predicted_position_ratio_;
			tracked_position_ratio_ = predicted_position_ratio_
			    + ball.tracker_alpha * residual;
			tracked_position_ratio_ =
			    std::clamp(tracked_position_ratio_, 0.0, 1.0);
			tracked_velocity_ratio_per_s_ +=
			    ball.tracker_beta * residual / dt;
			tracked_velocity_ratio_per_s_ =
			    std::clamp(tracked_velocity_ratio_per_s_,
			               -ball.tracker_max_speed_ratio_per_second,
			               ball.tracker_max_speed_ratio_per_second);
		}
		tracker_lost_frames_ = 0;
		acquire_confirm_count_ = 0;
		if(ball_lost_)
		{
			ETEST_LOG_INFO("BALL",
			               std::string("recovered after ")
			                   + std::to_string(ball_lost_frame_count_)
			                   + " lost frames");
			ball_lost_ = false;
		}
		ball_lost_frame_count_ = 0;
		last_ball_error_code_.clear();

		// axis from tracker
		double axis_px = inner_roi.x
		    + tracked_position_ratio_ * (inner_roi.width - 1);
		// inverse warp position
		cv::Point2f wp(axis_px, best->center.y + inner_roi.y);
		if(!inverse_warp_matrix_.empty())
		{
			std::vector<cv::Point2f> vi = {wp}, vo;
			cv::perspectiveTransform(vi, vo, inverse_warp_matrix_);
			r.x = vo[0].x;
			r.y = vo[0].y;
		}
		else
		{
			r.x = wp.x + locked_track_.bounding_roi.x;
			r.y = wp.y + locked_track_.bounding_roi.y;
		}

		if(ball_state_ == BallState::REACQUIRE_BALL)
			ball_state_ = BallState::TRACK_BALL;

		// zero calibration
		if(!zero_locked_)
		{
			if(ball_state_ != BallState::CALIBRATE_ZERO
			   && ball_state_ != BallState::TRACK_BALL)
				ball_state_ = BallState::CALIBRATE_ZERO;
			if(ball.zero_mode == "ratio")
			{
				zero_locked_ = true;
				ball_state_ = BallState::TRACK_BALL;
				goto ratio_zero;
			}
			else if(ball.zero_mode == "fixed")
			{
				zero_position_px_ = ball.zero_position_px;
				zero_locked_ = true;
				ball_state_ = BallState::TRACK_BALL;
			}
			else
			{
				if(!zero_buffer_.empty()
				   && std::abs(axis_px - zero_buffer_.back())
				       > ball.max_jump_px)
					zero_buffer_.clear();
				zero_buffer_.push_back(axis_px);
				while(static_cast<int>(zero_buffer_.size())
				      > ball.zero_samples)
					zero_buffer_.pop_front();
				if(static_cast<int>(zero_buffer_.size())
				   >= ball.zero_samples)
				{
					std::vector<double> v(zero_buffer_.begin(),
					                      zero_buffer_.end());
					std::sort(v.begin(), v.end());
					double med = v[v.size() / 2];
					if(v.back() - v.front() <= ball.zero_range_px)
					{
						zero_position_px_ = med;
						zero_locked_ = true;
						ball_state_ = BallState::TRACK_BALL;
					}
				}
			}
		}
		if(!zero_locked_)
		{
			r.error_code = "ZERO_CALIBRATING";
			return r;
		}

ratio_zero:
		if(ball.zero_mode == "ratio")
		{
			r.offset_mm = static_cast<int>(std::lround(
			    (tracked_position_ratio_ - ball.zero_position_ratio)
			    * ball.pipe_length_mm));
		}
		else
		{
			double eff_w = ball.pipe_warp_width
			    * (1.0 - 2.0 * ball.pipe_inner_margin_x_ratio);
			double mpp = eff_w > 0
			    ? ball.pipe_length_mm / eff_w
			    : ball.pipe_length_mm / ball.pipe_warp_width;
			double raw_mm = (axis_px - zero_position_px_) * mpp;
			r.offset_mm = static_cast<int>(std::lround(raw_mm));
		}

		double conf = best->quality;
		if(tracker_initialized_)
		{
			double gf = best_gate_error / std::max(gate, 0.001);
			conf *= (1.0 - gf * 0.5);
		}
		conf = std::clamp(conf, 0.0, 1.0);
		r.valid = true;
		r.calibrated = true;
		r.confidence = conf;
		r.error_code.clear();
		return r;
	}

	// ── resizeLetterbox ──
	void VisionProcessor::resizeLetterbox(const cv::Mat& source,
	                                      cv::Mat& destination,
	                                      cv::Scalar bg)
	{
		if(source.empty())
		{
			destination = cv::Mat();
			return;
		}
		double scale = std::min(
		    static_cast<double>(destination.cols) / source.cols,
		    static_cast<double>(destination.rows) / source.rows);
		int nw = static_cast<int>(source.cols * scale),
		    nh = static_cast<int>(source.rows * scale);
		cv::Mat tmp;
		cv::resize(source, tmp, cv::Size(nw, nh), 0, 0,
		           cv::INTER_LINEAR);
		destination.setTo(bg);
		int ox = (destination.cols - nw) / 2,
		    oy = (destination.rows - nh) / 2;
		if(tmp.channels() == destination.channels())
			tmp.copyTo(destination(cv::Rect(ox, oy, nw, nh)));
		else
		{
			cv::Mat dt;
			cv::cvtColor(tmp, dt, cv::COLOR_GRAY2BGR);
			dt.copyTo(destination(cv::Rect(ox, oy, nw, nh)));
		}
	}

	// ── drawDebugInfo ──
	void VisionProcessor::drawDebugInfo(
	    cv::Mat& frame, const VisionResult& result) noexcept
	{
		try
		{
			if(frame.empty())
				return;
			if(result.target_type == "BALL")
			{
				drawBallDebugInfo(frame, result);
				return;
			}
			cv::Point c(frame.cols / 2, frame.rows / 2);
			cv::drawMarker(frame, c, cv::Scalar(255, 0, 0),
			               cv::MARKER_CROSS, 20, 2);
			if(!result.valid)
			{
				cv::putText(frame, "Target: LOST", cv::Point(20, 30),
				            cv::FONT_HERSHEY_SIMPLEX, 0.7,
				            cv::Scalar(0, 0, 255), 2);
				return;
			}
			cv::Point t(static_cast<int>(result.x),
			            static_cast<int>(result.y));
			cv::circle(frame, t, 8, cv::Scalar(0, 255, 0), 2);
			cv::line(frame, c, t, cv::Scalar(0, 255, 0), 2);
			cv::putText(frame, "Target: FOUND", cv::Point(20, 30),
			            cv::FONT_HERSHEY_SIMPLEX, 0.7,
			            cv::Scalar(0, 255, 0), 2);
		}
		catch(const cv::Exception& e)
		{
			ETEST_LOG_ERROR("VISION", std::string("draw:") + e.what());
		}
		catch(...)
		{
			ETEST_LOG_ERROR("VISION", "draw unknown");
		}
	}

	void VisionProcessor::drawBallDebugInfo(
	    cv::Mat& frame, const VisionResult& result) noexcept
	{
		try
		{
			auto& ball = config_.ball;
			int kW = ball.work_width, kH = ball.work_height;
			cv::Mat wv;
			if(frame.cols != kW || frame.rows != kH)
				cv::resize(frame, wv, cv::Size(kW, kH), 0, 0,
				           cv::INTER_LINEAR);
			else
				wv = frame.clone();
			int disp_w = std::max(kW / 2, ball.pipe_warp_width / 2),
			    disp_h = kH / 2;
			cv::Mat canvas(cv::Size(kW + disp_w, kH), wv.type(),
			               cv::Scalar(0, 0, 0));
			cv::Mat top_left = canvas(cv::Rect(0, 0, kW, kH / 2));
			cv::Mat annotated = wv.clone();
			if(track_locked_ && locked_pipe_points_valid_)
			{
				std::array<cv::Point, 4> pi;
				for(int i = 0; i < 4; ++i)
					pi[i] = cv::Point(
					    static_cast<int>(locked_pipe_points_[i].x),
					    static_cast<int>(locked_pipe_points_[i].y));
				for(int i = 0; i < 4; ++i)
					cv::line(annotated, pi[i], pi[(i + 1) % 4],
					         cv::Scalar(0, 255, 0), 2);
				cv::Point ap1(
				    static_cast<int>((locked_pipe_points_[0].x
				                      + locked_pipe_points_[3].x)
				                     * 0.5F),
				    static_cast<int>((locked_pipe_points_[0].y
				                      + locked_pipe_points_[3].y)
				                     * 0.5F));
				cv::Point ap2(
				    static_cast<int>((locked_pipe_points_[1].x
				                      + locked_pipe_points_[2].x)
				                     * 0.5F),
				    static_cast<int>((locked_pipe_points_[1].y
				                      + locked_pipe_points_[2].y)
				                     * 0.5F));
				cv::line(annotated, ap1, ap2, cv::Scalar(0, 255, 255),
				         1);
			}
			const char* tl = "TRACK_LOST";
			if(track_locked_)
				tl = (track_lost_frame_count_ > 0) ? "TRACK_PREDICT"
				                                   : "TRACK_OK";
			cv::putText(annotated, tl, cv::Point(5, 60),
			            cv::FONT_HERSHEY_SIMPLEX, 0.5,
			            cv::Scalar(0, 255, 0), 1);
			if(result.valid && result.calibrated)
			{
				cv::circle(annotated,
				           cv::Point(static_cast<int>(result.x),
				                     static_cast<int>(result.y)),
				           8, cv::Scalar(0, 0, 255), -1);
				std::string lb = "BALL "
				    + std::to_string(result.offset_mm) + "mm "
				    + std::to_string(static_cast<int>(
				        std::lround(result.confidence * 100)))
				    + "%";
				cv::putText(annotated, lb, cv::Point(5, 20),
				            cv::FONT_HERSHEY_SIMPLEX, 0.7,
				            cv::Scalar(0, 255, 0), 2);
			}
			else
			{
				const char* st = "FIND_TRACK";
				switch(ball_state_)
				{
				case BallState::CALIBRATE_ZERO:
					st = "CALIB";
					break;
				case BallState::TRACK_BALL:
					st = "TRACK";
					break;
				case BallState::REACQUIRE_BALL:
					st = "REACQ";
					break;
				default:
					break;
				}
				cv::putText(annotated, st, cv::Point(5, 20),
				            cv::FONT_HERSHEY_SIMPLEX, 0.7,
				            cv::Scalar(0, 165, 255), 2);
				if(!result.error_code.empty())
					cv::putText(annotated, result.error_code,
					            cv::Point(5, 45),
					            cv::FONT_HERSHEY_SIMPLEX, 0.5,
					            cv::Scalar(0, 0, 255), 1);
			}
			cv::resize(annotated, top_left,
			           cv::Size(top_left.cols, top_left.rows), 0, 0,
			           cv::INTER_AREA);
			// top_right: warped pipe with letterbox
			cv::Mat top_right = canvas(cv::Rect(kW, 0, disp_w, disp_h));
			if(!debug_warped_pipe_.empty())
				resizeLetterbox(debug_warped_pipe_, top_right);
			// bottom_left
			cv::Mat bottom_left =
			    canvas(cv::Rect(0, kH / 2, kW, kH / 2));
			if(!debug_track_mask_.empty())
			{
				cv::Mat mc;
				cv::cvtColor(debug_track_mask_, mc, cv::COLOR_GRAY2BGR);
				resizeLetterbox(mc, bottom_left);
			}
			// bottom_right: overlay on binary in original size, then letterbox
			cv::Mat bottom_right =
			    canvas(cv::Rect(kW, kH / 2, disp_w, kH / 2));
			cv::Mat overlay;
			if(!debug_ball_binary_.empty())
			{
				cv::cvtColor(debug_ball_binary_, overlay,
				             cv::COLOR_GRAY2BGR);
			}
			else
			{
				overlay = cv::Mat(cv::Size(ball.pipe_warp_width,
				                           ball.pipe_warp_height),
				                  CV_8UC3, cv::Scalar(0, 0, 0));
			}
			// draw candidates on overlay in original size
			int hough_cnt = 0, passed_cnt = 0, in_gate_cnt = 0;
			for(auto& c: debug_ball_candidates_)
			{
				++hough_cnt;
				cv::Scalar col;
				if(c.reject_code == 0 && !c.association_rejected)
				{
					++passed_cnt;
					col = cv::Scalar(0, 255, 0);
				} // green passed
				else if(c.reject_code == 0 && c.association_rejected)
				{
					++in_gate_cnt;
					col = cv::Scalar(255, 0, 255);
				} // purple gate-rejected
				else if(c.reject_code != 0 && c.association_rejected)
					col = cv::Scalar(0, 0, 255); // red
				else
					col = cv::Scalar(0, 255,
					                 255); // yellow appearance-pass
				cv::circle(overlay,
				           cv::Point(static_cast<int>(c.center.x),
				                     static_cast<int>(c.center.y)),
				           static_cast<int>(c.radius), col, 1);
				std::string an = "r"
				    + std::to_string(static_cast<int>(c.radius)) + " q"
				    + std::to_string(static_cast<int>(c.quality * 100));
				if(c.reject_code != 0)
					an += " R" + std::to_string(c.reject_code);
				cv::putText(overlay, an,
				            cv::Point(static_cast<int>(c.center.x) + 15,
				                      static_cast<int>(c.center.y)),
				            cv::FONT_HERSHEY_SIMPLEX, 0.35, col, 1);
			}
			cv::Mat debug_overlay;
			resizeLetterbox(overlay, debug_overlay,
			                cv::Scalar(0, 0, 0));
			debug_overlay.copyTo(bottom_right(cv::Rect(
			    0, 0, std::min(debug_overlay.cols, bottom_right.cols),
			    std::min(debug_overlay.rows, bottom_right.rows))));
			// status text
			std::string stxt;
			if(result.valid && result.calibrated)
				stxt = "OK offset=" + std::to_string(result.offset_mm)
				    + "mm conf="
				    + std::to_string(static_cast<int>(
				        std::lround(result.confidence * 100)))
				    + "%";
			else if(result.error_code == "ZERO_CALIBRATING")
				stxt = "CALIBRATING "
				    + std::to_string(zero_buffer_.size()) + "/"
				    + std::to_string(ball.zero_samples);
			else
				stxt = result.error_code;
			stxt += " h=" + std::to_string(hough_cnt)
			    + " p=" + std::to_string(passed_cnt)
			    + " g=" + std::to_string(in_gate_cnt);
			cv::putText(bottom_right, stxt, cv::Point(5, 20),
			            cv::FONT_HERSHEY_SIMPLEX, 0.7,
			            cv::Scalar(200, 200, 200), 1);
			cv::resize(canvas, frame, cv::Size(frame.cols, frame.rows),
			           0, 0, cv::INTER_LINEAR);
		}
		catch(const cv::Exception& e)
		{
			ETEST_LOG_ERROR("VISION",
			                std::string("ball draw:") + e.what());
		}
		catch(...)
		{
			ETEST_LOG_ERROR("VISION", "ball draw unknown");
		}
	}

	bool VisionProcessor::loadNnModel(const std::string& onnx,
	                                  const std::string& names,
	                                  double conf, double nms) noexcept
	{
		try
		{
			nn_net_ = cv::dnn::readNetFromONNX(onnx);
			if(nn_net_.empty())
			{
				ETEST_LOG_ERROR("VISION_NN", "load failed");
				nn_loaded_ = false;
				return false;
			}
			nn_confidence_threshold_ = conf;
			nn_nms_threshold_ = nms;
			nn_class_names_.clear();
			if(!names.empty())
			{
				std::ifstream f(names);
				std::string n;
				while(std::getline(f, n))
					if(!n.empty())
						nn_class_names_.push_back(n);
			}
			nn_output_names_ = nn_net_.getUnconnectedOutLayersNames();
			nn_loaded_ = true;
			return true;
		}
		catch(const cv::Exception& e)
		{
			ETEST_LOG_ERROR("VISION_NN",
			                std::string("onnx:") + e.what());
		}
		catch(...)
		{
			ETEST_LOG_ERROR("VISION_NN", "onnx unknown");
		}
		nn_loaded_ = false;
		return false;
	}

	cv::Mat VisionProcessor::detectNn(const cv::Mat& frame) noexcept
	{
		try
		{
			if(!nn_loaded_ || frame.empty())
				return frame.clone();
			constexpr int iw = 640, ih = 640;
			auto blob = cv::dnn::blobFromImage(
			    frame, 1.0 / 255.0, cv::Size(iw, ih), cv::Scalar(),
			    true, false);
			nn_net_.setInput(blob);
			std::vector<cv::Mat> outs;
			nn_net_.forward(outs, nn_output_names_);
			float fw = static_cast<float>(frame.cols),
			      fh = static_cast<float>(frame.rows), xs = fw / iw,
			      ys = fh / ih;
			std::vector<cv::Rect> boxes;
			std::vector<float> confs;
			std::vector<int> cids;
			for(auto& o: outs)
			{
				auto* d = reinterpret_cast<const float*>(o.data);
				int rs = o.size[1], cs = o.size[2];
				for(int r = 0; r < rs; ++r)
				{
					auto* rd = d + r * cs;
					float obj = rd[4];
					if(obj < nn_confidence_threshold_)
						continue;
					float mc = 0;
					int bi = 0;
					for(int c = 0; c < 80; ++c)
					{
						float cf = rd[5 + c];
						if(cf > mc)
						{
							mc = cf;
							bi = c;
						}
					}
					float fc = obj * mc;
					if(fc < nn_confidence_threshold_)
						continue;
					boxes.emplace_back(
					    static_cast<int>((rd[0] - 0.5f * rd[2]) * xs),
					    static_cast<int>((rd[1] - 0.5f * rd[3]) * ys),
					    static_cast<int>(rd[2] * xs),
					    static_cast<int>(rd[3] * ys));
					confs.push_back(fc);
					cids.push_back(bi);
				}
			}
			std::vector<int> nms;
			cv::dnn::NMSBoxes(boxes, confs, nn_confidence_threshold_,
			                  nn_nms_threshold_, nms);
			last_detections_.clear();
			cv::Mat res = frame.clone();
			for(int idx: nms)
			{
				auto& box = boxes[idx];
				int cid = cids[idx];
				float cf = confs[idx];
				std::string name = "class_" + std::to_string(cid);
				if(cid >= 0
				   && static_cast<std::size_t>(cid)
				       < nn_class_names_.size())
					name = nn_class_names_[cid];
				last_detections_.push_back(
				    {name, cf, box.x, box.y, box.x, box.y + box.height,
				     box.x + box.width, box.y + box.height,
				     box.x + box.width, box.y});
				cv::rectangle(res, box, cv::Scalar(0, 255, 0), 2);
				cv::putText(
				    res,
				    name + " "
				        + std::to_string(static_cast<int>(cf * 100))
				        + "%",
				    cv::Point(box.x, box.y - 5),
				    cv::FONT_HERSHEY_SIMPLEX, 0.5,
				    cv::Scalar(0, 255, 0), 1);
			}
			return res;
		}
		catch(const cv::Exception& e)
		{
			ETEST_LOG_ERROR("VISION_NN",
			                std::string("detectNn:") + e.what());
			return frame.clone();
		}
		catch(const std::exception& e)
		{
			ETEST_LOG_ERROR("VISION_NN",
			                std::string("detectNn:") + e.what());
			return frame.clone();
		}
		catch(...)
		{
			ETEST_LOG_ERROR("VISION_NN", "detectNn unknown");
			return frame.clone();
		}
	}

	bool VisionProcessor::isNnLoaded() const noexcept
	{
		return nn_loaded_;
	}
	const std::vector<DetectionInfo>&
	VisionProcessor::getLastDetections() const noexcept
	{
		return last_detections_;
	}

} // namespace etest::vision