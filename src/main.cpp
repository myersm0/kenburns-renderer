#include <cstdlib>
#include <chrono>
#include <sys/stat.h>
#include <unistd.h>
#include "slideshow.h"
#include "renderer.h"
#include "commands.h"

void write_pause_info(
	const std::string& dir, RenderParams& params,
	int output_width, int output_height
) {
	if (!params.valid) return;

	CropState crop = interpolate_crop(
		params.a_t, params.kf_a,
		params.pyramid_a->source_width,
		params.pyramid_a->source_height,
		output_width, output_height
	);

	double t = smoothstep(params.a_t);
	double zoom = params.kf_a.start_zoom
		+ (params.kf_a.end_zoom - params.kf_a.start_zoom) * t;

	std::string tmp = dir + "/pause_info.json.tmp";
	std::string final_path = dir + "/pause_info.json";

	std::ofstream file(tmp);
	if (!file.is_open()) return;

	file << "{"
		<< "\"t\":" << params.a_t
		<< ",\"zoom\":" << zoom
		<< ",\"center_x\":" << crop.center_x
		<< ",\"center_y\":" << crop.center_y
		<< ",\"crop_w\":" << crop.crop_w
		<< ",\"crop_h\":" << crop.crop_h
		<< ",\"source_w\":" << params.pyramid_a->source_width
		<< ",\"source_h\":" << params.pyramid_a->source_height
		<< ",\"kf_start_x\":" << params.kf_a.start_x
		<< ",\"kf_start_y\":" << params.kf_a.start_y
		<< ",\"kf_start_zoom\":" << params.kf_a.start_zoom
		<< ",\"kf_end_x\":" << params.kf_a.end_x
		<< ",\"kf_end_y\":" << params.kf_a.end_y
		<< ",\"kf_end_zoom\":" << params.kf_a.end_zoom
		<< ",\"curved\":" << (params.kf_a.curved ? "true" : "false");

	if (params.kf_a.curved) {
		file << ",\"ctrl1_x\":" << params.kf_a.ctrl1_x
			<< ",\"ctrl1_y\":" << params.kf_a.ctrl1_y
			<< ",\"ctrl2_x\":" << params.kf_a.ctrl2_x
			<< ",\"ctrl2_y\":" << params.kf_a.ctrl2_y;
	}

	file << ",\"alpha\":" << params.alpha
		<< "}" << std::endl;

	file.close();
	std::rename(tmp.c_str(), final_path.c_str());
}

int main(int argc, char** argv) {
	if (argc < 2) {
		printf("usage: kbr <command_dir> [options]\n");
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

	const char* window_name = "kbr";
	cv::namedWindow(window_name, cv::WINDOW_NORMAL);
	cv::moveWindow(window_name, 0, 0);
	cv::setWindowProperty(window_name, cv::WND_PROP_FULLSCREEN, cv::WINDOW_FULLSCREEN);

	SlideshowState state(fps, hold_seconds, fade_seconds);
	state.output_width = output_width;
	state.output_height = output_height;

	Renderer renderer(output_width, output_height);
	CommandReader commands(command_dir);
	StatusWriter status_writer(command_dir);
	KeyWriter key_writer(command_dir);

	const int key_escape = 27;
	const int key_quit = 113;
	const int key_pause = 32;
	const int key_debug = 63;

	auto last_command_time = std::chrono::steady_clock::now();
	bool paused = false;
	bool debug = false;
	RenderParams last_params;

	SlideshowPhase prev_phase = SlideshowPhase::Idle;
	bool prev_preload = false;
	bool prev_fade_complete = false;
	bool prev_paused = false;
	bool status_dirty = true;

	auto write_status = [&]() {
		std::string phase_str;
		switch (state.get_phase()) {
		case SlideshowPhase::Idle: phase_str = "idle"; break;
		case SlideshowPhase::Holding: phase_str = "holding"; break;
		case SlideshowPhase::Transitioning: phase_str = "transitioning"; break;
		}
		status_writer.write(phase_str, state.preload_ready(),
			state.fade_complete(), paused);
	};

	write_status();

	while (true) {
		Command cmd = commands.poll();

		if (cmd.type != CommandType::None) {
			last_command_time = std::chrono::steady_clock::now();
			status_dirty = true;
		}

		switch (cmd.type) {
		case CommandType::Load:
			if (cmd.has_style)
				state.load_with_style(cmd.path, cmd.style);
			else
				state.load(cmd.path, cmd.kf);
			break;
		case CommandType::Transition:
			state.start_transition();
			break;
		case CommandType::Swap:
			state.swap();
			break;
		case CommandType::Cancel:
			state.cancel_transition();
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

		if (!paused)
			last_params = state.tick();
		int keypress = renderer.render(
			window_name, last_params, state.blur_strength, wait_ms, debug);

		if (keypress == key_pause) {
			paused = !paused;
			status_dirty = true;
			if (paused)
				write_pause_info(command_dir, last_params, output_width, output_height);
		} else if (keypress == key_debug) {
			debug = !debug;
		} else if (keypress == key_escape || keypress == key_quit) {
			_exit(0);
		} else if (keypress >= 0) {
			key_writer.write(keypress);
		}

		// detect state changes and write status
		SlideshowPhase cur_phase = state.get_phase();
		bool cur_preload = state.preload_ready();
		bool cur_fade_complete = state.fade_complete();

		if (cur_phase != prev_phase || cur_preload != prev_preload ||
				cur_fade_complete != prev_fade_complete ||
				paused != prev_paused || status_dirty) {
			write_status();
			prev_phase = cur_phase;
			prev_preload = cur_preload;
			prev_fade_complete = cur_fade_complete;
			prev_paused = paused;
			status_dirty = false;
		}

		// idle timeout
		auto elapsed = std::chrono::steady_clock::now() - last_command_time;
		double idle_seconds = std::chrono::duration<double>(elapsed).count();
		if (idle_seconds > idle_timeout_seconds)
			_exit(0);
	}
}
