#include <opencv2/opencv.hpp>
#include <iostream>

namespace
{
	int h_min = 12, h_max = 33;
	int s_min = 43, s_max = 255;
	int v_min = 6, v_max = 255;

	void on_trackbar(int, void *) {}
} // namespace

int main()
{
	cv::VideoCapture cap("docs/videos/test5.mp4");
	if(!cap.isOpened())
	{
		std::cerr << "Failed to open video\n";
		return 1;
	}

	cv::namedWindow("controls", cv::WINDOW_NORMAL);
	cv::namedWindow("frame", cv::WINDOW_NORMAL);
	cv::namedWindow("mask", cv::WINDOW_NORMAL);
	cv::namedWindow("morph", cv::WINDOW_NORMAL);

	cv::createTrackbar("H min", "controls", nullptr, 179, on_trackbar);
	cv::setTrackbarPos("H min", "controls", h_min);
	cv::createTrackbar("H max", "controls", nullptr, 179, on_trackbar);
	cv::setTrackbarPos("H max", "controls", h_max);
	cv::createTrackbar("S min", "controls", nullptr, 255, on_trackbar);
	cv::setTrackbarPos("S min", "controls", s_min);
	cv::createTrackbar("S max", "controls", nullptr, 255, on_trackbar);
	cv::setTrackbarPos("S max", "controls", s_max);
	cv::createTrackbar("V min", "controls", nullptr, 255, on_trackbar);
	cv::setTrackbarPos("V min", "controls", v_min);
	cv::createTrackbar("V max", "controls", nullptr, 255, on_trackbar);
	cv::setTrackbarPos("V max", "controls", v_max);

	cv::Mat frame;

	while(true)
	{
		if(!cap.read(frame) || frame.empty())
		{
			cap.set(cv::CAP_PROP_POS_FRAMES, 0);
			continue;
		}

		h_min = cv::getTrackbarPos("H min", "controls");
		h_max = cv::getTrackbarPos("H max", "controls");
		s_min = cv::getTrackbarPos("S min", "controls");
		s_max = cv::getTrackbarPos("S max", "controls");
		v_min = cv::getTrackbarPos("V min", "controls");
		v_max = cv::getTrackbarPos("V max", "controls");

		// 保持 16:9，不能缩放成 640x480
		cv::resize(frame, frame, cv::Size(640, 360));

		// 按当前视频调整
		cv::Rect roi(25, 143, 586, 72);
		roi &= cv::Rect(0, 0, frame.cols, frame.rows);

		if(roi.width <= 0 || roi.height <= 0)
		{
			std::cerr << "Invalid ROI\n";
			continue;
		}

		cv::Mat roi_image = frame(roi);
		cv::Mat hsv;
		cv::Mat mask;

		cv::cvtColor(roi_image, hsv, cv::COLOR_BGR2HSV);

		cv::inRange(hsv, cv::Scalar(h_min, s_min, v_min),
		            cv::Scalar(h_max, s_max, v_max), mask);

		cv::Mat morph;

		cv::morphologyEx(
		    mask, morph, cv::MORPH_CLOSE,
		    cv::getStructuringElement(cv::MORPH_RECT, cv::Size(21, 5)));

		cv::morphologyEx(morph, morph, cv::MORPH_OPEN,
		                 cv::getStructuringElement(cv::MORPH_ELLIPSE,
		                                           cv::Size(3, 3)));

		cv::Mat display = frame.clone();
		cv::rectangle(display, roi, cv::Scalar(0, 0, 255), 2);

		cv::imshow("frame", display);
		cv::imshow("mask", mask);
		cv::imshow("morph", morph);

		const int key = cv::waitKey(30);

		if(key == 27 || key == 'q')
		{
			break;
		}

		if(key == ' ')
		{
			cv::waitKey(0);
		}

		if(key == 'p')
		{
			std::cout << "H=[" << h_min << ", " << h_max << "] "
			          << "S=[" << s_min << ", " << s_max << "] "
			          << "V=[" << v_min << ", " << v_max << "]\n";
		}
	}

	return 0;
}
