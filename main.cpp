// main.cpp
#include <cstdlib>
#include <chrono>
#include <sys/stat.h>
#include <unistd.h>
#include "slideshow.h"
#include "renderer.h"
#include "commands.h"

int main(int argc, char** argv) {
	if (argc < 2) {
		printf("usage: slideshow <command_dir> [options]\n");
		printf("  --width W --height H --fps F --hold S --fade S --timeout S\n");
		return 1;
	}

	std::string command_dir = argv[1];
	mkdir(command_dir.c_str(), 0755);

	int fps = 30;
	double hold_seconds = 5.0;
	double fade_seconds = 3.0;
	double idle_timeout_seconds = 300.0;
	int output_width = 1920;
	int output_height = 1080;

	for (int i = 2; i < argc - 1; i += 2) {
		std::string flag = argv[i];
		if (flag == "--width") output_width = atoi(argv[i + 1]);
		else if (flag == "--height") output_height = atoi(argv[i + 1]);
		else if (flag == "--fps") fps = atoi(argv[i + 1]);
		else if (flag == "--hold") hold_seconds = atof(argv[i + 1]);
		else if (flag == "--fade") fade_seconds = atof(argv[i + 1]);
		else if (flag == "--timeout") idle_timeout_seconds = atof(argv[i + 1]);
	}

	int wait_ms = 1000 / fps;

	const char* window_name = "slideshow";
	cv::namedWindow(window_name, cv::WINDOW_NORMAL);
	cv::moveWindow(window_name, 0, 0);
	cv::setWindowProperty(window_name, cv::WND_PROP_FULLSCREEN, cv::WINDOW_FULLSCREEN);

	SlideshowState state(fps, hold_seconds, fade_seconds);
	state.output_width = output_width;
	state.output_height = output_height;

	Renderer renderer(output_width, output_height);
	CommandReader commands(command_dir);
	StatusWriter status_writer(command_dir);
	EventWriter events(command_dir);

	std::string current_image_path;
	SlideshowPhase prev_phase = SlideshowPhase::Idle;
	bool prev_preload = false;
	auto last_command_time = std::chrono::steady_clock::now();

	while (true) {
		Command cmd = commands.poll();

		if (cmd.type != CommandType::None)
			last_command_time = std::chrono::steady_clock::now();

		switch (cmd.type) {
		case CommandType::Load:
			if (cmd.has_style)
				state.load_with_style(cmd.path, cmd.style);
			else
				state.load(cmd.path, cmd.kf);
			if (state.get_phase() == SlideshowPhase::Holding)
				current_image_path = cmd.path;
			break;
		case CommandType::Transition:
			state.start_transition();
			break;
		case CommandType::Skip:
			state.skip();
			events.write_event("skipped");
			break;
		case CommandType::Config:
			if (cmd.config_key == "blur")
				state.blur_strength = cmd.config_value;
			else if (cmd.config_key == "hold")
				state.set_hold(cmd.config_value, fps);
			else if (cmd.config_key == "fade")
				state.set_fade(cmd.config_value, fps);
			break;
		case CommandType::Quit:
			_exit(0);
		case CommandType::None:
			break;
		}

		RenderParams params = state.tick();
		int keypress = renderer.render(
			window_name, params, state.blur_strength, wait_ms);

		// edge-triggered key events
		if (keypress >= 0)
			events.write_key(keypress);

		// edge-triggered phase changes
		SlideshowPhase cur_phase = state.get_phase();
		if (cur_phase != prev_phase) {
			switch (cur_phase) {
			case SlideshowPhase::Idle:
				events.write_phase("idle"); break;
			case SlideshowPhase::Holding:
				events.write_phase("holding"); break;
			case SlideshowPhase::Transitioning:
				events.write_phase("transitioning"); break;
			}
			prev_phase = cur_phase;
		}

		// edge-triggered preload ready
		bool cur_preload = state.preload_ready();
		if (cur_preload && !prev_preload)
			events.write_event("preload_ready");
		prev_preload = cur_preload;

		// status file for debugging/monitoring
		Status status;
		switch (cur_phase) {
		case SlideshowPhase::Idle: status.phase = "idle"; break;
		case SlideshowPhase::Holding: status.phase = "holding"; break;
		case SlideshowPhase::Transitioning: status.phase = "transitioning"; break;
		}
		status.image = current_image_path;
		status.last_key = keypress;
		status.preload_ready = cur_preload;
		status_writer.write(status);

		// idle timeout
		auto elapsed = std::chrono::steady_clock::now() - last_command_time;
		double idle_seconds = std::chrono::duration<double>(elapsed).count();
		if (idle_seconds > idle_timeout_seconds)
			_exit(0);

		// ESC
		if (keypress == 27)
			_exit(0);
	}
}
