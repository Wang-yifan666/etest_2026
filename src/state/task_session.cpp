#include "state/task_session.hpp"

#include <chrono>
#include <string>

namespace etest
{

void TaskSession::reset()
{
	// session_id 递增，其他全部清零
	++session_id;
	mode = TaskMode::NONE;
	phase = ContestTaskPhase::IDLE;
	tracking_mode = TrackingMode::NONE;
	command_tag.clear();
	vision_enabled = false;
	start_sent = false;
	measurement_valid = false;
	target_position_mm = 0.0;
	current_position_mm = 0.0;
	last_confidence = 0.0;
	last_global_center = {-1.0F, -1.0F};
	seq = 0;
	vsession_confirmed = false;
	calibration_valid_frames = 0;
	calibration_start_time = {};
	lost_frames = 0;
	center_stable_frames = 0;
	filtered_x = 0.0;
	filtered_y = 0.0;
	current_ball_global_x = 0.0;
	current_ball_global_y = 0.0;
}

void TaskSession::enterPhase(ContestTaskPhase new_phase)
{
	phase = new_phase;
	calibration_start_time = std::chrono::steady_clock::now();
	calibration_valid_frames = 0;
}

TaskMode TaskSession::modeFromTag(const std::string& tag)
{
	if(tag == "M0001") return TaskMode::Q2;
	if(tag == "M0002") return TaskMode::Q3;
	if(tag == "M0003") return TaskMode::Q4;
	if(tag == "M0004") return TaskMode::Q5;
	if(tag == "M0005") return TaskMode::Q6;
	return TaskMode::NONE;
}

const char* TaskSession::modeName(TaskMode m)
{
	switch(m)
	{
	case TaskMode::Q2: return "H2";
	case TaskMode::Q3: return "H3";
	case TaskMode::Q4: return "H4";
	case TaskMode::Q5: return "H5";
	case TaskMode::Q6: return "H6";
	default: return "";
	}
}

} // namespace etest