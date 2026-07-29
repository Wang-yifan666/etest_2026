#include "vision/vision.hpp"

#include "core/logger.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <exception>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

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
		const auto now = std::chrono::steady_clock::now();
		const auto timestamp_ms =
		    std::chrono::duration_cast<std::chrono::milliseconds>(
		        now.time_since_epoch()).count();

		if(frame.empty())
		{
			if(!empty_frame_reported_)
			{
				ETEST_LOG_ERROR("VISION", "received an empty frame");
				empty_frame_reported_ = true;
			}
			VisionResult result;
			result.frame_id = frame_id_counter_;
			result.timestamp_ms = static_cast<std::int64_t>(timestamp_ms);
			result.target_type = (mode == VisionMode::Ball) ? "BALL" : "";
			result.error_code = "EMPTY_FRAME";
			return result;
		}
		if(empty_frame_reported_)
		{
			ETEST_LOG_INFO("VISION", "valid frame input recovered");
			empty_frame_reported_ = false;
		}

		VisionResult result;
		result.frame_id = frame_id_counter_;
		result.timestamp_ms = static_cast<std::int64_t>(timestamp_ms);
		if(mode == VisionMode::Ball) result.target_type = "BALL";

		try
		{
			switch(mode)
			{
			case VisionMode::ColorTarget:
				result = detectColorTarget(frame);
				result.frame_id = frame_id_counter_;
				result.timestamp_ms = static_cast<std::int64_t>(timestamp_ms);
				return result;
			case VisionMode::Ball:
				result = detectBall(frame);
				result.frame_id = frame_id_counter_;
				result.timestamp_ms = static_cast<std::int64_t>(timestamp_ms);
				return result;
			default:
				result.error_code = "UNSUPPORTED_MODE";
				return result;
			}
		}
		catch(const cv::Exception& error)
		{
			ETEST_LOG_ERROR("VISION", std::string("OpenCV exception: ") + error.what());
			result.valid = false; result.error_code = "CV_EXCEPTION"; return result;
		}
		catch(const std::exception& error)
		{
			ETEST_LOG_ERROR("VISION", std::string("exception: ") + error.what());
			result.valid = false; result.error_code = "STD_EXCEPTION"; return result;
		}
		catch(...)
		{
			ETEST_LOG_ERROR("VISION", "unknown exception");
			result.valid = false; result.error_code = "UNKNOWN_EXCEPTION"; return result;
		}
	}

	VisionResult VisionProcessor::detectColorTarget(const cv::Mat& frame)
	{
		VisionResult result; result.target_type = "RED_TARGET";
		cv::Mat hsv; cv::cvtColor(frame, hsv, cv::COLOR_BGR2HSV);
		cv::Mat mask1, mask2;
		cv::inRange(hsv, cv::Scalar(config_.red_h1_min, config_.saturation_min, config_.value_min),
		            cv::Scalar(config_.red_h1_max, 255, 255), mask1);
		cv::inRange(hsv, cv::Scalar(config_.red_h2_min, config_.saturation_min, config_.value_min),
		            cv::Scalar(config_.red_h2_max, 255, 255), mask2);
		cv::Mat mask = mask1 | mask2;
		const cv::Mat kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE,
		    cv::Size(config_.morphology_kernel, config_.morphology_kernel));
		cv::morphologyEx(mask, mask, cv::MORPH_OPEN, kernel);
		cv::morphologyEx(mask, mask, cv::MORPH_CLOSE, kernel);
		std::vector<std::vector<cv::Point>> contours;
		cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
		if(contours.empty()) { result.error_code = "NO_CONTOUR"; return result; }
		const auto largest = std::max_element(contours.begin(), contours.end(),
		    [](const auto& l, const auto& r) { return cv::contourArea(l) < cv::contourArea(r); });
		const double area = cv::contourArea(*largest);
		if(area < config_.min_area) { result.error_code = "AREA_TOO_SMALL"; return result; }
		const cv::Moments moments = cv::moments(*largest);
		if(moments.m00 == 0.0) { result.error_code = "ZERO_MOMENT"; return result; }
		result.valid = true; result.x = moments.m10 / moments.m00; result.y = moments.m01 / moments.m00;
		result.confidence = 1.0;
		const cv::RotatedRect rect = cv::minAreaRect(*largest);
		result.angle = rect.angle; result.distance = std::sqrt(area);
		return result;
	}

	TrackResult VisionProcessor::detectBrownPipe(const cv::Mat& frame) noexcept
	{
		TrackResult result;
		try
		{
			if(frame.empty()) { ETEST_LOG_ERROR("PIPE", "empty frame"); return result; }
			const auto& ball = config_.ball;
			cv::Rect search_roi(ball.pipe_search_roi_x, ball.pipe_search_roi_y,
			    ball.pipe_search_roi_w, ball.pipe_search_roi_h);
			search_roi &= cv::Rect(0, 0, frame.cols, frame.rows);
			if(search_roi.empty() || search_roi.area() < 100)
			{
				debug_track_mask_ = cv::Mat::zeros(frame.size(), CV_8UC1);
				return result;
			}
			const double roi_area = static_cast<double>(search_roi.area());
			const cv::Mat search_image = frame(search_roi);
			cv::Mat hsv; cv::cvtColor(search_image, hsv, cv::COLOR_BGR2HSV);
			cv::Mat brown_mask;
			cv::inRange(hsv, cv::Scalar(ball.brown_h_min, ball.brown_s_min, ball.brown_v_min),
			            cv::Scalar(ball.brown_h_max, ball.brown_s_max, ball.brown_v_max), brown_mask);
			const cv::Mat close_kernel = cv::getStructuringElement(cv::MORPH_RECT,
			    cv::Size(ball.pipe_close_kernel_w, ball.pipe_close_kernel_h));
			cv::morphologyEx(brown_mask, brown_mask, cv::MORPH_CLOSE, close_kernel);
			const cv::Mat open_kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE,
			    cv::Size(ball.pipe_open_kernel, ball.pipe_open_kernel));
			cv::morphologyEx(brown_mask, brown_mask, cv::MORPH_OPEN, open_kernel);
			debug_track_mask_ = cv::Mat::zeros(frame.size(), CV_8UC1);
			brown_mask.copyTo(debug_track_mask_(search_roi));
			std::vector<std::vector<cv::Point>> contours;
			cv::findContours(brown_mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
			const int total_contours = static_cast<int>(contours.size());
			int rej_area = 0, rej_aspect = 0, rej_angle = 0, rej_fill = 0, rej_shortside = 0;
			double best_score = -1.0;
			cv::RotatedRect best_rect;
			double best_area = 0, best_aspect = 0, best_angle = 0, best_fill = 0, best_short = 0;
			for(const auto& contour: contours)
			{
				const double area = cv::contourArea(contour);
				if(area < roi_area * ball.pipe_min_area_ratio) { ++rej_area; continue; }
				const cv::RotatedRect rect = cv::minAreaRect(contour);
				const double long_side = std::max(rect.size.width, rect.size.height);
				const double short_side = std::min(rect.size.width, rect.size.height);
				if(short_side < ball.pipe_min_short_side_px) { ++rej_shortside; continue; }
				const double aspect = long_side / short_side;
				if(aspect < ball.pipe_min_aspect_ratio) { ++rej_aspect; continue; }
				cv::Point2f pts[4]; rect.points(pts);
				const cv::Point2f e1 = pts[1] - pts[0], e2 = pts[2] - pts[1];
				const cv::Point2f long_edge = cv::norm(e1) >= cv::norm(e2) ? e1 : e2;
				const double angle_deg = std::atan2(long_edge.y, long_edge.x) * 180.0 / CV_PI;
				const double hdev = std::min(std::abs(angle_deg), std::abs(180.0 - std::abs(angle_deg)));
				if(hdev > ball.pipe_horizontal_angle_max) { ++rej_angle; continue; }
				const double ra = rect.size.width * rect.size.height;
				const double fill = (ra > 0.0) ? area / ra : 0.0;
				if(fill < ball.pipe_min_fill_ratio) { ++rej_fill; continue; }
				const double score = area * std::min(aspect, 20.0) * fill;
				if(score > best_score) { best_score = score; best_rect = rect; best_area = area; best_aspect = aspect;
					best_angle = angle_deg; best_fill = fill; best_short = short_side; }
			}
			// throttle log (1s)
			{
				static auto last_log = std::chrono::steady_clock::now();
				auto now = std::chrono::steady_clock::now();
				auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_log);
				if(ms.count() >= 1000)
				{
					ETEST_LOG_INFO("PIPE_DEBUG", "roi=" + std::to_string(search_roi.x) + "," +
					    std::to_string(search_roi.y) + " " + std::to_string(search_roi.width) + "x" +
					    std::to_string(search_roi.height) + " mask_nz=" + std::to_string(cv::countNonZero(brown_mask)) +
					    " contours=" + std::to_string(total_contours) + " rej(area=" + std::to_string(rej_area) +
					    " aspect=" + std::to_string(rej_aspect) + " angle=" + std::to_string(rej_angle) +
					    " fill=" + std::to_string(rej_fill) + " short=" + std::to_string(rej_shortside) + ")" +
					    " best(area=" + std::to_string(best_area) + " aspect=" + std::to_string(best_aspect) +
					    " angle=" + std::to_string(best_angle) + " fill=" + std::to_string(best_fill) +
					    " short=" + std::to_string(best_short) + ") stable=" + std::to_string(track_stable_count_));
					last_log = now;
				}
			}
			if(best_score < 0.0) return result;
			best_rect.center.x += search_roi.x; best_rect.center.y += search_roi.y;
			result.valid = true; result.rect = best_rect;
			result.bounding_roi = cv::Rect(static_cast<int>(best_rect.center.x - best_rect.size.width / 2),
			    static_cast<int>(best_rect.center.y - best_rect.size.height / 2),
			    static_cast<int>(best_rect.size.width), static_cast<int>(best_rect.size.height));
			result.bounding_roi &= cv::Rect(0, 0, frame.cols, frame.rows);
			result.confidence = std::clamp(best_score / (roi_area * 20.0), 0.0, 1.0);
			cv::Point2f points[4]; best_rect.points(points);
			double e01 = cv::norm(points[1] - points[0]), e12 = cv::norm(points[2] - points[1]);
			if(e01 >= e12)
			{ result.axis_p1 = (points[0] + points[3]) * 0.5F; result.axis_p2 = (points[1] + points[2]) * 0.5F; }
			else
			{ result.axis_p1 = (points[0] + points[1]) * 0.5F; result.axis_p2 = (points[2] + points[3]) * 0.5F; }
			return result;
		}
		catch(const cv::Exception& e) { ETEST_LOG_ERROR("PIPE", std::string("cv:") + e.what()); }
		catch(const std::exception& e) { ETEST_LOG_ERROR("PIPE", std::string("ex:") + e.what()); }
		catch(...) { ETEST_LOG_ERROR("PIPE", "unknown"); }
		return result;
	}

	bool VisionProcessor::isTrackSimilar(const TrackResult& a, const TrackResult& b) const noexcept
	{
		if(!a.valid || !b.valid) return false;
		const auto& ball = config_.ball;
		return cv::norm(a.rect.center - b.rect.center) < ball.pipe_similarity_center_max_px &&
		    std::abs(std::max(a.rect.size.width, a.rect.size.height) -
		             std::max(b.rect.size.width, b.rect.size.height)) < ball.pipe_similarity_length_max_px;
	}

	cv::Rect VisionProcessor::makeInnerRoi(const cv::Rect& track_roi, const cv::Size& work_size) noexcept
	{
		int mx = std::max(1, static_cast<int>(track_roi.width * 0.03));
		int my = std::max(1, static_cast<int>(track_roi.height * 0.15));
		cv::Rect inner(track_roi.x + mx, track_roi.y + my,
		    std::max(10, track_roi.width - mx * 2), std::max(10, track_roi.height - my * 2));
		inner &= cv::Rect(0, 0, work_size.width, work_size.height);
		if(inner.width < 10 || inner.height < 10) return cv::Rect();
		return inner;
	}

	bool VisionProcessor::orderTrackCorners(const cv::RotatedRect& rect,
	    std::array<cv::Point2f, 4>& ordered) noexcept
	{
		try
		{
			cv::Point2f pts[4]; rect.points(pts);
			std::sort(pts, pts + 4, [](const cv::Point2f& a, const cv::Point2f& b) { return (a.x + a.y) < (b.x + b.y); });
			cv::Point2f tl = (pts[0].x <= pts[1].x) ? pts[0] : pts[1];
			cv::Point2f tr = (pts[0].x <= pts[1].x) ? pts[1] : pts[0];
			cv::Point2f bl = (pts[2].x <= pts[3].x) ? pts[2] : pts[3];
			cv::Point2f br = (pts[2].x <= pts[3].x) ? pts[3] : pts[2];
			double area = std::abs((tr.x - tl.x) * (br.y - tl.y) - (tr.y - tl.y) * (br.x - tl.x));
			if(area < 100.0)
			{
				auto now = std::chrono::steady_clock::now();
				if(std::chrono::duration_cast<std::chrono::milliseconds>(now - last_corner_order_error_time_).count() >= 1000)
				{ ETEST_LOG_ERROR("PIPE", "orderTrackCorners: degenerate quad area=" + std::to_string(area)); last_corner_order_error_time_ = now; }
				return false;
			}
			double tl2 = cv::norm(tr - tl), bl2 = cv::norm(br - bl), ll2 = cv::norm(bl - tl), rl2 = cv::norm(br - tr);
			if(tl2 < 1.0 || bl2 < 1.0 || ll2 < 1.0 || rl2 < 1.0)
			{
				auto now = std::chrono::steady_clock::now();
				if(std::chrono::duration_cast<std::chrono::milliseconds>(now - last_corner_order_error_time_).count() >= 1000)
				{ ETEST_LOG_ERROR("PIPE", "orderTrackCorners: edge too short"); last_corner_order_error_time_ = now; }
				return false;
			}
			if((tl.x + bl.x) > (tr.x + br.x)) { std::swap(tl, tr); std::swap(bl, br); }
			ordered[0] = tl; ordered[1] = tr; ordered[2] = br; ordered[3] = bl;
			return true;
		}
		catch(const cv::Exception& e) {
			auto now = std::chrono::steady_clock::now();
			if(std::chrono::duration_cast<std::chrono::milliseconds>(now - last_corner_order_error_time_).count() >= 1000)
			{ ETEST_LOG_ERROR("PIPE", std::string("orderTrackCorners cv:") + e.what()); last_corner_order_error_time_ = now; }
		} catch(...) {
			auto now = std::chrono::steady_clock::now();
			if(std::chrono::duration_cast<std::chrono::milliseconds>(now - last_corner_order_error_time_).count() >= 1000)
			{ ETEST_LOG_ERROR("PIPE", "orderTrackCorners unknown"); last_corner_order_error_time_ = now; }
		}
		return false;
	}

	bool VisionProcessor::updateWarpMatrices() noexcept
	{
		try
		{
			if(!locked_pipe_points_valid_) return false;
			const auto& ball = config_.ball;
			int ww = ball.pipe_warp_width, wh = ball.pipe_warp_height;
			const cv::Point2f dst[4] = { {0,0}, {static_cast<float>(ww-1),0},
			    {static_cast<float>(ww-1),static_cast<float>(wh-1)}, {0,static_cast<float>(wh-1)} };
			cv::Mat nw = cv::getPerspectiveTransform(locked_pipe_points_.data(), dst);
			cv::Mat ni = cv::getPerspectiveTransform(dst, locked_pipe_points_.data());
			if(nw.empty() || ni.empty()) goto fail;
			for(int r=0;r<3;++r) for(int c=0;c<3;++c)
				if(!std::isfinite(nw.at<double>(r,c)) || !std::isfinite(ni.at<double>(r,c))) goto fail;
			if(std::abs(cv::determinant(nw)) < 1e-8) goto fail;
			warp_matrix_ = nw; inverse_warp_matrix_ = ni; return true;
		fail:
			{ auto now = std::chrono::steady_clock::now();
			  if(std::chrono::duration_cast<std::chrono::milliseconds>(now - last_warp_update_error_time_).count() >= 1000)
			  { ETEST_LOG_ERROR("PIPE", "updateWarpMatrices failed; keeping previous"); last_warp_update_error_time_ = now; } }
			return false;
		}
		catch(const cv::Exception& e) {
			auto now = std::chrono::steady_clock::now();
			if(std::chrono::duration_cast<std::chrono::milliseconds>(now - last_warp_update_error_time_).count() >= 1000)
			{ ETEST_LOG_ERROR("PIPE", std::string("updateWarp:") + e.what()); last_warp_update_error_time_ = now; }
		} catch(...) {
			auto now = std::chrono::steady_clock::now();
			if(std::chrono::duration_cast<std::chrono::milliseconds>(now - last_warp_update_error_time_).count() >= 1000)
			{ ETEST_LOG_ERROR("PIPE", "updateWarp unknown"); last_warp_update_error_time_ = now; }
		}
		return false;
	}

	std::vector<BallCandidate> VisionProcessor::detectBallCandidates(const cv::Mat& warped_roi) noexcept
	{
		std::vector<BallCandidate> candidates;
		try
		{
			if(warped_roi.empty() || warped_roi.cols < 10 || warped_roi.rows < 10) return candidates;
			const auto& ball = config_.ball;
			cv::Mat gray;
			if(warped_roi.channels() == 3) cv::cvtColor(warped_roi, gray, cv::COLOR_BGR2GRAY);
			else gray = warped_roi.clone();
			cv::GaussianBlur(gray, gray, cv::Size(7, 7), 1.5);
			std::vector<cv::Vec3f> circles;
			cv::HoughCircles(gray, circles, cv::HOUGH_GRADIENT, ball.hough_dp, ball.hough_min_distance,
			    ball.hough_param1, ball.hough_param2, ball.ball_min_radius, ball.ball_max_radius);
			if(circles.empty()) return candidates;
			const double roi_h = static_cast<double>(warped_roi.rows);
			const double roi_w = static_cast<double>(warped_roi.cols);
			const double min_y = roi_h * ball.ball_min_center_y_ratio;
			const double max_y = roi_h * ball.ball_max_center_y_ratio;
			const double expected_y = roi_h * ball.ball_expected_center_y_ratio;
			const double expected_r = ball.ball_expected_radius;
			for(const auto& c: circles)
			{
				float cx=c[0], cy=c[1], cr=c[2];
				if(cx - cr < 0 || cx + cr >= roi_w || cy - cr < 0 || cy + cr >= roi_h) continue;
				if(cy < min_y || cy > max_y) continue;
				if(cr < static_cast<float>(ball.ball_min_radius) || cr > static_cast<float>(ball.ball_max_radius)) continue;
				int bx = std::max(0, static_cast<int>(cx - cr)), by = std::max(0, static_cast<int>(cy - cr));
				int bw = std::min(static_cast<int>(cx + cr), warped_roi.cols) - bx;
				int bh = std::min(static_cast<int>(cy + cr), warped_roi.rows) - by;
				if(bw <= 0 || bh <= 0) continue;
				cv::Rect bbox(bx, by, bw, bh); cv::Mat local_gray = gray(bbox);
				cv::Mat inner_mask = cv::Mat::zeros(bbox.size(), CV_8UC1);
				cv::circle(inner_mask, cv::Point(static_cast<int>(cx-bx), static_cast<int>(cy-by)),
				    static_cast<int>(cr*0.70F), 255, -1);
				double mean_inner = cv::mean(local_gray, inner_mask)[0];
				cv::Mat ring_mask = cv::Mat::zeros(bbox.size(), CV_8UC1);
				cv::circle(ring_mask, cv::Point(static_cast<int>(cx-bx), static_cast<int>(cy-by)),
				    static_cast<int>(cr*1.30F), 255, -1);
				cv::Mat inner_ring = cv::Mat::zeros(bbox.size(), CV_8UC1);
				cv::circle(inner_ring, cv::Point(static_cast<int>(cx-bx), static_cast<int>(cy-by)),
				    static_cast<int>(cr*0.95F), 255, -1);
				cv::Mat ring_only = ring_mask - inner_ring;
				double mean_ring = cv::mean(local_gray, ring_only)[0];
				double ring_contrast = mean_ring - mean_inner;
				if(mean_inner > ball.ball_max_inner_gray) continue;
				if(ring_contrast < ball.ball_min_ring_contrast) continue;
				double rs = 1.0 - std::clamp(std::abs(cr - expected_r) / expected_r, 0.0, 1.0);
				double cs = 1.0 - std::clamp(std::abs(cy - expected_y) / (roi_h * 0.5), 0.0, 1.0);
				double ds = 1.0 - std::clamp(mean_inner / 255.0, 0.0, 1.0);
				double cts = std::clamp((ring_contrast + 128.0) / 128.0, 0.0, 1.0);
				double quality = 0.30 * rs + 0.25 * cs + 0.25 * ds + 0.20 * cts;
				if(quality < ball.ball_min_quality) continue;
				BallCandidate bc;
				bc.center = cv::Point2f(cx, cy); bc.radius = cr;
				bc.normalized_x = cx / (roi_w - 1.0); bc.mean_inner_gray = mean_inner;
				bc.mean_ring_gray = mean_ring; bc.ring_contrast = ring_contrast;
				bc.radius_score = rs; bc.center_score = cs; bc.darkness_score = ds;
				bc.contrast_score = cts; bc.quality = quality; bc.passed = true;
				candidates.push_back(bc);
			}
			std::sort(candidates.begin(), candidates.end(),
			    [](const BallCandidate& a, const BallCandidate& b) { return a.quality > b.quality; });
		}
		catch(const cv::Exception& e) { ETEST_LOG_ERROR("BALL", std::string("detectBallCandidates: ") + e.what()); }
		catch(...) { ETEST_LOG_ERROR("BALL", "detectBallCandidates unknown"); }
		return candidates;
	}

	// ===== detectBall with alpha-beta tracker =====
	VisionResult VisionProcessor::detectBall(const cv::Mat& frame)
	{
		VisionResult result; result.target_type = "BALL";
		const auto& ball = config_.ball;
		int kW = ball.work_width, kH = ball.work_height;
		// Letterbox
		cv::Mat work_frame;
		double scale = std::min(static_cast<double>(kW)/frame.cols, static_cast<double>(kH)/frame.rows);
		int sw = static_cast<int>(frame.cols*scale), sh = static_cast<int>(frame.rows*scale);
		cv::Mat scaled; cv::resize(frame, scaled, cv::Size(sw, sh), 0, 0, cv::INTER_LINEAR);
		work_frame = cv::Mat(kH, kW, frame.type(), cv::Scalar(0,0,0));
		int ox = (kW - sw)/2, oy = (kH - sh)/2;
		scaled.copyTo(work_frame(cv::Rect(ox, oy, sw, sh)));
		// Fixed mode
		if(ball.pipe_mode == "fixed")
		{
			const cv::Point2f src[4] = {
			    {static_cast<float>(ball.pipe_fixed_tl_x), static_cast<float>(ball.pipe_fixed_tl_y)},
			    {static_cast<float>(ball.pipe_fixed_tr_x), static_cast<float>(ball.pipe_fixed_tr_y)},
			    {static_cast<float>(ball.pipe_fixed_br_x), static_cast<float>(ball.pipe_fixed_br_y)},
			    {static_cast<float>(ball.pipe_fixed_bl_x), static_cast<float>(ball.pipe_fixed_bl_y)}};
			double a = std::abs((src[1].x-src[0].x)*(src[3].y-src[0].y)-(src[1].y-src[0].y)*(src[3].x-src[0].x));
			if(a < 100.0) { ETEST_LOG_ERROR("VISION", "fixed pipe area too small"); result.error_code = "TRACK_CONFIG_INVALID"; return result; }
			if(!warp_locked_)
			{
				int ww = ball.pipe_warp_width, wh = ball.pipe_warp_height;
				const cv::Point2f dst[4] = {{0,0},{static_cast<float>(ww-1),0},{static_cast<float>(ww-1),static_cast<float>(wh-1)},{0,static_cast<float>(wh-1)}};
				warp_matrix_ = cv::getPerspectiveTransform(src, dst);
				if(warp_matrix_.empty() || std::isnan(warp_matrix_.at<double>(0,0))) { ETEST_LOG_ERROR("VISION", "fixed warp failed"); result.error_code = "PIPE_WARP_FAILED"; return result; }
				warp_locked_ = true; track_locked_ = true;
				locked_track_.valid = true; locked_track_.bounding_roi = cv::Rect(0, 0, kW, kH);
				locked_track_.axis_p1 = src[0]; locked_track_.axis_p2 = src[1];
				ETEST_LOG_INFO("VISION", "fixed pipe mode activated");
			}
			goto warp_and_detect;
		}
		// Auto mode pipe detection + dynamic warp
		{
			const TrackResult track = detectBrownPipe(work_frame);
			if(!track_locked_)
			{
				if(track.valid)
				{
					if(isTrackSimilar(track, locked_track_))
					{
						++track_stable_count_;
						if(track_stable_count_ >= ball.pipe_stable_frames)
						{
							std::array<cv::Point2f,4> ordered;
							if(orderTrackCorners(track.rect, ordered))
							{
								locked_pipe_points_ = ordered; locked_pipe_points_valid_ = true;
								if(updateWarpMatrices()) { track_locked_ = true; locked_track_ = track; warp_locked_ = true;
									track_stable_count_ = 0; track_lost_frame_count_ = 0;
									ETEST_LOG_INFO("VISION", "pipe geometry locked (auto dynamic)"); }
							}
						}
					}
					else { track_stable_count_ = 1; locked_track_ = track; }
				}
				else { track_stable_count_ = 0; }
			}
			else
			{
				if(track.valid && isTrackSimilar(track, locked_track_))
				{
					if(track_lost_frame_count_ > 0)
					{
						auto now = std::chrono::steady_clock::now();
						if(std::chrono::duration_cast<std::chrono::milliseconds>(now - last_pipe_recovered_info_time_).count() >= 1000)
						{ ETEST_LOG_INFO("VISION", "pipe recovered"); last_pipe_recovered_info_time_ = now; }
						track_lost_frame_count_ = 0;
					}
					locked_track_ = track;
					std::array<cv::Point2f,4> det_pts;
					if(orderTrackCorners(track.rect, det_pts))
					{
						if(ball.pipe_update_each_frame)
						{
							double alpha = std::clamp(ball.pipe_geometry_alpha, 0.01, 1.0);
							for(int i=0;i<4;++i)
							{
								locked_pipe_points_[i].x = static_cast<float>(alpha*det_pts[i].x + (1.0-alpha)*locked_pipe_points_[i].x);
								locked_pipe_points_[i].y = static_cast<float>(alpha*det_pts[i].y + (1.0-alpha)*locked_pipe_points_[i].y);
							}
						}
						else locked_pipe_points_ = det_pts;
						updateWarpMatrices();
					}
				}
				else
				{
					if(track_lost_frame_count_ == 0)
					{
						auto now = std::chrono::steady_clock::now();
						if(std::chrono::duration_cast<std::chrono::milliseconds>(now - last_pipe_lost_warn_time_).count() >= 1000)
						{ ETEST_LOG_WARN("VISION", "pipe lost"); last_pipe_lost_warn_time_ = now; }
					}
					++track_lost_frame_count_;
					if(track_lost_frame_count_ >= ball.pipe_lost_timeout_frames)
					{
						track_locked_ = false; track_stable_count_ = 0; track_lost_frame_count_ = 0; warp_locked_ = false;
						locked_pipe_points_valid_ = false; ball_state_ = BallState::FIND_TRACK;
						zero_locked_ = false; zero_buffer_.clear();
						tracker_initialized_ = false; tracker_lost_frames_ = 0; reacquire_confirm_count_ = 0;
						ETEST_LOG_WARN("VISION", "pipe lost timeout; resetting to FIND_TRACK");
					}
				}
			}
		}
		if(!track_locked_) { result.error_code = "PIPE_NOT_STABLE"; return result; }

	warp_and_detect:
		cv::Mat warped;
		cv::warpPerspective(work_frame, warped, warp_matrix_,
		    cv::Size(ball.pipe_warp_width, ball.pipe_warp_height), cv::INTER_LINEAR);
		debug_warped_pipe_ = warped.clone();
		int mx = static_cast<int>(ball.pipe_warp_width * ball.pipe_inner_margin_x_ratio);
		int my = static_cast<int>(ball.pipe_warp_height * ball.pipe_inner_margin_y_ratio);
		cv::Rect inner_roi(mx, my, std::max(1, ball.pipe_warp_width - 2*mx), std::max(1, ball.pipe_warp_height - 2*my));
		inner_roi &= cv::Rect(0, 0, warped.cols, warped.rows);
		if(inner_roi.empty() || inner_roi.width < 10 || inner_roi.height < 5) { result.error_code = "PIPE_ROI_INVALID"; return result; }
		const cv::Mat roi_img = warped(inner_roi);
		// Canny debug
		{
			cv::Mat gray2;
			if(roi_img.channels() == 3) cv::cvtColor(roi_img, gray2, cv::COLOR_BGR2GRAY); else gray2 = roi_img.clone();
			cv::Canny(gray2, debug_ball_binary_, 50, 150);
		}
		auto candidates = detectBallCandidates(roi_img);
		debug_ball_candidates_ = candidates;

		auto throttleBallError = [this](const std::string& code, const std::string& tag, const std::string& msg) {
			auto now = std::chrono::steady_clock::now();
			auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_ball_error_time_);
			if(code != last_ball_error_code_) {
				ETEST_LOG_WARN(tag.c_str(), msg.c_str());
				last_ball_error_code_ = code; last_ball_error_time_ = now;
				ball_lost_frames_on_recover_ = ball_lost_frame_count_;
			} else if(elapsed.count() >= 2000) {
				ETEST_LOG_WARN(tag.c_str(), msg.c_str()); last_ball_error_time_ = now;
			}
		};

		// ── Tracker prediction ──
		auto now = std::chrono::steady_clock::now();
		double dt = 0.02;
		if(tracker_initialized_)
		{
			dt = std::chrono::duration<double>(now - tracker_last_time_).count();
			dt = std::clamp(dt, 0.005, 0.100);
			predicted_position_ratio_ = tracked_position_ratio_ + tracked_velocity_ratio_per_s_ * dt;
			predicted_position_ratio_ = std::clamp(predicted_position_ratio_, 0.0, 1.0);
		}
		tracker_last_time_ = now;

		// ── Gate association ──
		bool in_global_reacquire = (tracker_lost_frames_ >= ball.tracker_global_reacquire_frames);

		if(candidates.empty())
		{
			// no candidates
			if(tracker_initialized_)
			{
				++tracker_lost_frames_;
				++ball_lost_frame_count_;
				if(!ball_lost_) { ball_lost_ = true; last_ball_lost_log_time_ = now; }
			}
			if(tracker_lost_frames_ > ball.tracker_max_predict_frames)
				result.valid = false;
			else
				result.valid = false;
			result.error_code = "NO_CIRCLE_CANDIDATE";
			throttleBallError("NO_CIRCLE_CANDIDATE", "BALL", "no circle candidate");
			return result;
		}

		// Gate
		double gate = ball.tracker_gate_ratio * (1.0 + ball.tracker_gate_growth_per_lost_frame * tracker_lost_frames_);
		gate = std::min(gate, ball.tracker_max_gate_ratio);

		const BallCandidate* best = nullptr;
		double best_assoc_score = -1e9;
		double best_gate_error = 0.0;

		for(const auto& c: candidates)
		{
			if(!c.passed) continue;
			double delta = std::abs(c.normalized_x - predicted_position_ratio_);
			if(!in_global_reacquire && tracker_initialized_ && delta > gate) continue;
			double assoc = c.quality - 2.0 * delta / std::max(gate, 0.001);
			if(assoc > best_assoc_score) { best_assoc_score = assoc; best = &c; best_gate_error = delta; }
		}

		if(best == nullptr)
		{
			if(tracker_initialized_)
			{
				++tracker_lost_frames_;
				++ball_lost_frame_count_;
				if(!ball_lost_) { ball_lost_ = true; last_ball_lost_log_time_ = now; }
			}
			throttleBallError("ALL_CANDIDATES_REJECTED", "BALL", "all candidates rejected by gate");
			result.error_code = "ALL_CANDIDATES_REJECTED";
			return result;
		}

		// ── Reacquire confirm ──
		if(in_global_reacquire)
		{
			if(reacquire_confirm_count_ == 0)
			{
				reacquire_candidate_ratio_ = best->normalized_x;
				reacquire_confirm_count_ = 1;
				++ball_lost_frame_count_;
				result.error_code = "BALL_LOST";
				return result;
			}
			if(std::abs(best->normalized_x - reacquire_candidate_ratio_) < gate)
			{
				++reacquire_confirm_count_;
				reacquire_candidate_ratio_ = best->normalized_x;
			}
			else
			{
				reacquire_confirm_count_ = 1;
				reacquire_candidate_ratio_ = best->normalized_x;
			}
			if(reacquire_confirm_count_ < ball.reacquire_confirm_frames)
			{
				++ball_lost_frame_count_;
				result.error_code = "BALL_LOST";
				return result;
			}
			// Reacquire confirmed — reset tracker
			tracker_lost_frames_ = 0;
			reacquire_confirm_count_ = 0;
			tracker_initialized_ = false; // will be re-initialized below
		}

		// ── Tracker update ──
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
			tracked_position_ratio_ = predicted_position_ratio_ + ball.tracker_alpha * residual;
			tracked_position_ratio_ = std::clamp(tracked_position_ratio_, 0.0, 1.0);
			tracked_velocity_ratio_per_s_ += ball.tracker_beta * residual / dt;
			tracked_velocity_ratio_per_s_ = std::clamp(tracked_velocity_ratio_per_s_,
			    -ball.tracker_max_speed_ratio_per_second, ball.tracker_max_speed_ratio_per_second);
		}
		tracker_lost_frames_ = 0;
		reacquire_confirm_count_ = 0;

		// Recovery log
		if(ball_lost_)
		{
			ETEST_LOG_INFO("BALL", std::string("ball recovered after ") + std::to_string(ball_lost_frame_count_) + " lost frames");
			ball_lost_ = false;
		}
		ball_lost_frame_count_ = 0;
		last_ball_error_code_.clear();

		// ── Position output (inverse warp) ──
		cv::Point2f best_center = best->center;
		double best_quality = best->quality;
		double axis_position_px = best_center.x + inner_roi.x;

		// warp coordinates
		cv::Point2f warped_pt(best_center.x + inner_roi.x, best_center.y + inner_roi.y);
		if(!inverse_warp_matrix_.empty())
		{
			std::vector<cv::Point2f> v_in = {warped_pt};
			std::vector<cv::Point2f> v_out;
			cv::perspectiveTransform(v_in, v_out, inverse_warp_matrix_);
			result.x = v_out[0].x;
			result.y = v_out[0].y;
		}
		else
		{
			result.x = warped_pt.x + locked_track_.bounding_roi.x;
			result.y = warped_pt.y + locked_track_.bounding_roi.y;
		}

		if(ball_state_ == BallState::REACQUIRE_BALL) ball_state_ = BallState::TRACK_BALL;

		// Zero calibration
		if(!zero_locked_)
		{
			if(ball_state_ != BallState::CALIBRATE_ZERO && ball_state_ != BallState::TRACK_BALL)
				ball_state_ = BallState::CALIBRATE_ZERO;
			if(ball.zero_mode == "fixed") { zero_position_px_ = ball.zero_position_px; zero_locked_ = true; ball_state_ = BallState::TRACK_BALL; }
			else
			{
				if(!zero_buffer_.empty() && std::abs(axis_position_px - zero_buffer_.back()) > ball.max_jump_px)
					zero_buffer_.clear();
				zero_buffer_.push_back(axis_position_px);
				while(static_cast<int>(zero_buffer_.size()) > ball.zero_samples) zero_buffer_.pop_front();
				if(static_cast<int>(zero_buffer_.size()) >= ball.zero_samples)
				{
					std::vector<double> vals(zero_buffer_.begin(), zero_buffer_.end());
					std::sort(vals.begin(), vals.end());
					double med = vals[vals.size()/2];
					if(vals.back() - vals.front() <= ball.zero_range_px)
					{ zero_position_px_ = med; zero_locked_ = true; ball_state_ = BallState::TRACK_BALL; }
				}
			}
		}
		if(!zero_locked_) { result.error_code = "ZERO_CALIBRATING"; return result; }

		// mm calculation
		double eff_w = ball.pipe_warp_width * (1.0 - 2.0 * ball.pipe_inner_margin_x_ratio);
		double mm_per_px = eff_w > 0.0 ? ball.pipe_length_mm / eff_w : ball.pipe_length_mm / ball.pipe_warp_width;
		double raw_mm = (axis_position_px - zero_position_px_) * mm_per_px;

		// Confidence (tracker-based, no filter_alpha low-pass)
		double conf = best_quality;
		if(tracker_initialized_)
		{
			double gate_frac = best_gate_error / std::max(gate, 0.001);
			conf *= (1.0 - gate_frac * 0.5);
		}
		conf = std::clamp(conf, 0.0, 1.0);

		result.valid = true;
		result.calibrated = true;
		result.confidence = conf;
		result.offset_mm = static_cast<int>(std::lround(raw_mm));
		result.error_code.clear();
		return result;
	}

	void VisionProcessor::drawDebugInfo(cv::Mat& frame, const VisionResult& result) noexcept
	{
		try
		{
			if(frame.empty()) return;
			if(result.target_type == "BALL") { drawBallDebugInfo(frame, result); return; }
			cv::Point c(frame.cols/2, frame.rows/2);
			cv::drawMarker(frame, c, cv::Scalar(255,0,0), cv::MARKER_CROSS, 20, 2);
			if(!result.valid) { cv::putText(frame, "Target: LOST", cv::Point(20,30), cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0,0,255), 2); return; }
			cv::Point t(static_cast<int>(result.x), static_cast<int>(result.y));
			cv::circle(frame, t, 8, cv::Scalar(0,255,0), 2);
			cv::line(frame, c, t, cv::Scalar(0,255,0), 2);
			cv::putText(frame, "Target: FOUND", cv::Point(20,30), cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0,255,0), 2);
		}
		catch(const cv::Exception& e) { ETEST_LOG_ERROR("VISION", std::string("draw:") + e.what()); }
		catch(...) { ETEST_LOG_ERROR("VISION", "draw unknown"); }
	}

	void VisionProcessor::drawBallDebugInfo(cv::Mat& frame, const VisionResult& result) noexcept
	{
		try
		{
			const auto& ball = config_.ball; int kW = ball.work_width, kH = ball.work_height;
			cv::Mat work_view;
			if(frame.cols != kW || frame.rows != kH) cv::resize(frame, work_view, cv::Size(kW,kH), 0,0, cv::INTER_LINEAR);
			else work_view = frame.clone();
			int disp_w = std::max(kW/2, ball.pipe_warp_width/2), disp_h = kH/2;
			cv::Mat canvas(cv::Size(kW+disp_w, kH), work_view.type(), cv::Scalar(0,0,0));
			cv::Mat top_left = canvas(cv::Rect(0,0,kW,kH/2));
			cv::Mat annotated = work_view.clone();
			if(track_locked_ && locked_pipe_points_valid_)
			{
				std::array<cv::Point,4> pi;
				for(int i=0;i<4;++i) pi[i] = cv::Point(static_cast<int>(locked_pipe_points_[i].x), static_cast<int>(locked_pipe_points_[i].y));
				for(int i=0;i<4;++i) cv::line(annotated, pi[i], pi[(i+1)%4], cv::Scalar(0,255,0), 2);
				cv::Point ap1(static_cast<int>((locked_pipe_points_[0].x+locked_pipe_points_[3].x)*0.5F),
				    static_cast<int>((locked_pipe_points_[0].y+locked_pipe_points_[3].y)*0.5F));
				cv::Point ap2(static_cast<int>((locked_pipe_points_[1].x+locked_pipe_points_[2].x)*0.5F),
				    static_cast<int>((locked_pipe_points_[1].y+locked_pipe_points_[2].y)*0.5F));
				cv::line(annotated, ap1, ap2, cv::Scalar(0,255,255), 1);
			}
			const char* tl = "TRACK_LOST"; if(track_locked_) tl = (track_lost_frame_count_>0)?"TRACK_PREDICT":"TRACK_OK";
			cv::putText(annotated, tl, cv::Point(5,60), cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0,255,0), 1);
			if(result.valid && result.calibrated)
			{
				cv::circle(annotated, cv::Point(static_cast<int>(result.x), static_cast<int>(result.y)), 8, cv::Scalar(0,0,255), -1);
				std::string label = "BALL " + std::to_string(result.offset_mm) + "mm " + std::to_string(static_cast<int>(std::lround(result.confidence*100))) + "%";
				cv::putText(annotated, label, cv::Point(5,20), cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0,255,0), 2);
			}
			else
			{
				const char* st = "FIND_TRACK"; switch(ball_state_) { case BallState::CALIBRATE_ZERO: st="CALIB"; break; case BallState::TRACK_BALL: st="TRACK"; break; case BallState::REACQUIRE_BALL: st="REACQ"; break; default: break; }
				cv::putText(annotated, st, cv::Point(5,20), cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0,165,255), 2);
				if(!result.error_code.empty()) cv::putText(annotated, result.error_code, cv::Point(5,45), cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0,0,255), 1);
			}
			cv::resize(annotated, top_left, cv::Size(top_left.cols, top_left.rows), 0,0, cv::INTER_AREA);
			cv::Mat top_right = canvas(cv::Rect(kW,0,disp_w,disp_h));
			if(!debug_warped_pipe_.empty()) cv::resize(debug_warped_pipe_, top_right, cv::Size(top_right.cols,top_right.rows), 0,0, cv::INTER_AREA);
			cv::Mat bottom_left = canvas(cv::Rect(0,kH/2,kW,kH/2));
			if(!debug_track_mask_.empty()) { cv::Mat mc; cv::cvtColor(debug_track_mask_, mc, cv::COLOR_GRAY2BGR); cv::resize(mc, bottom_left, cv::Size(bottom_left.cols,bottom_left.rows), 0,0, cv::INTER_NEAREST); }
			cv::Mat bottom_right = canvas(cv::Rect(kW,kH/2,disp_w,kH/2));
			if(!debug_ball_binary_.empty()) { cv::Mat bc; cv::cvtColor(debug_ball_binary_, bc, cv::COLOR_GRAY2BGR); cv::resize(bc, bottom_right, cv::Size(bottom_right.cols,bottom_right.rows), 0,0, cv::INTER_NEAREST); }
			for(const auto& c: debug_ball_candidates_)
			{
				cv::Scalar col = c.passed ? cv::Scalar(0,255,0) : cv::Scalar(0,0,255);
				cv::circle(bottom_right, cv::Point(static_cast<int>(c.center.x), static_cast<int>(c.center.y)), static_cast<int>(c.radius), col, 1);
				std::string ann = "r="+std::to_string(static_cast<int>(c.radius))+" q="+std::to_string(static_cast<int>(c.quality*100))+" c="+std::to_string(static_cast<int>(c.ring_contrast));
				cv::putText(bottom_right, ann, cv::Point(static_cast<int>(c.center.x)+15, static_cast<int>(c.center.y)), cv::FONT_HERSHEY_SIMPLEX, 0.35, col, 1);
			}
			std::string stxt;
			if(result.valid && result.calibrated) stxt = "OK offset="+std::to_string(result.offset_mm)+"mm conf="+std::to_string(static_cast<int>(std::lround(result.confidence*100)))+"%";
			else if(result.error_code == "ZERO_CALIBRATING") stxt = "CALIBRATING "+std::to_string(zero_buffer_.size())+"/"+std::to_string(ball.zero_samples);
			else if(result.error_code == "NO_CIRCLE_CANDIDATE") stxt = "NO CIRCLE CANDIDATE";
			else stxt = result.error_code;
			cv::putText(bottom_right, stxt, cv::Point(5,20), cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(200,200,200), 1);
			cv::resize(canvas, frame, cv::Size(frame.cols, frame.rows), 0,0, cv::INTER_LINEAR);
		}
		catch(const cv::Exception& e) { ETEST_LOG_ERROR("VISION", std::string("ball draw:") + e.what()); }
		catch(...) { ETEST_LOG_ERROR("VISION", "ball draw unknown"); }
	}

	bool VisionProcessor::loadNnModel(const std::string& onnx_path, const std::string& names_path, double conf, double nms) noexcept
	{
		try
		{
			nn_net_ = cv::dnn::readNetFromONNX(onnx_path);
			if(nn_net_.empty()) { ETEST_LOG_ERROR("VISION_NN", "failed to load ONNX: "+onnx_path); nn_loaded_=false; return false; }
			nn_confidence_threshold_=conf; nn_nms_threshold_=nms; nn_class_names_.clear();
			if(!names_path.empty()) { std::ifstream f(names_path); std::string n; while(std::getline(f,n)) if(!n.empty()) nn_class_names_.push_back(n); }
			nn_output_names_=nn_net_.getUnconnectedOutLayersNames(); nn_loaded_=true; return true;
		}
		catch(const cv::Exception& e) { ETEST_LOG_ERROR("VISION_NN", std::string("ONNX:")+e.what()); }
		catch(...) { ETEST_LOG_ERROR("VISION_NN", "ONNX unknown"); }
		nn_loaded_=false; return false;
	}

	cv::Mat VisionProcessor::detectNn(const cv::Mat& frame) noexcept
	{
		try
		{
			if(!nn_loaded_ || frame.empty()) return frame.clone();
			constexpr int iw=640, ih=640;
			cv::Mat blob = cv::dnn::blobFromImage(frame, 1.0/255.0, cv::Size(iw,ih), cv::Scalar(), true, false);
			nn_net_.setInput(blob); std::vector<cv::Mat> outputs; nn_net_.forward(outputs, nn_output_names_);
			float fw=static_cast<float>(frame.cols), fh=static_cast<float>(frame.rows), xs=fw/iw, ys=fh/ih;
			std::vector<cv::Rect> boxes; std::vector<float> confs; std::vector<int> cids;
			for(const auto& o: outputs) {
				const auto* d = reinterpret_cast<const float*>(o.data); int rs=o.size[1], cs=o.size[2];
				for(int r=0;r<rs;++r) { const float* rd=d+r*cs; float obj=rd[4]; if(obj<nn_confidence_threshold_) continue;
					float mc=0; int bi=0; for(int c=0;c<80;++c) { float cf=rd[5+c]; if(cf>mc){mc=cf;bi=c;} }
					float fc=obj*mc; if(fc<nn_confidence_threshold_) continue;
					float cx=rd[0],cy=rd[1],w2=rd[2],h2=rd[3];
					boxes.emplace_back(static_cast<int>((cx-0.5F*w2)*xs), static_cast<int>((cy-0.5F*h2)*ys), static_cast<int>(w2*xs), static_cast<int>(h2*ys));
					confs.push_back(fc); cids.push_back(bi);
				}
			}
			std::vector<int> nms; cv::dnn::NMSBoxes(boxes, confs, nn_confidence_threshold_, nn_nms_threshold_, nms);
			last_detections_.clear(); cv::Mat res = frame.clone();
			for(int idx: nms) {
				const auto& box = boxes[idx]; int cid=cids[idx]; float cf=confs[idx];
				std::string name="class_"+std::to_string(cid);
				if(cid>=0&&static_cast<std::size_t>(cid)<nn_class_names_.size()) name=nn_class_names_[cid];
				last_detections_.push_back({name,cf,box.x,box.y,box.x,box.y+box.height,box.x+box.width,box.y+box.height,box.x+box.width,box.y});
				cv::rectangle(res, box, cv::Scalar(0,255,0), 2);
				cv::putText(res, name+" "+std::to_string(static_cast<int>(cf*100))+"%", cv::Point(box.x,box.y-5), cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0,255,0), 1);
			}
			return res;
		}
		catch(const cv::Exception& e) { ETEST_LOG_ERROR("VISION_NN", std::string("detectNn:")+e.what()); return frame.clone(); }
		catch(const std::exception& e) { ETEST_LOG_ERROR("VISION_NN", std::string("detectNn:")+e.what()); return frame.clone(); }
		catch(...) { ETEST_LOG_ERROR("VISION_NN", "detectNn unknown"); return frame.clone(); }
	}

	bool VisionProcessor::isNnLoaded() const noexcept { return nn_loaded_; }
	const std::vector<DetectionInfo>& VisionProcessor::getLastDetections() const noexcept { return last_detections_; }

} // namespace etest::vision