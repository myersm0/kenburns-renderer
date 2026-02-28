#pragma once
#include <string>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <sys/stat.h>
#include "slideshow.h"
#include <sys/event.h>
#include <fcntl.h>

enum class CommandType {
	None, Load, Transition, Skip, Quit, Config
};

struct Command {
	CommandType type = CommandType::None;
	std::string path;
	Keyframe kf;
	KeyframeParams style;
	bool has_raw_keyframe = false;
	bool has_style = false;
	std::string config_key;
	double config_value = 0.0;
};

struct Status {
	std::string phase;
	std::string image;
	double progress = 0.0;
	int last_key = -1;
	int frame = 0;
	bool preload_ready = false;
};

class CommandReader {
	std::string command_path;
	int kq;
	int dir_fd;

public:
	CommandReader(const std::string& dir) {
		command_path = dir + "/command.json";

		kq = kqueue();
		dir_fd = open(dir.c_str(), O_RDONLY);

		struct kevent change;
		EV_SET(&change, dir_fd, EVFILT_VNODE,
			EV_ADD | EV_ENABLE | EV_CLEAR,
			NOTE_WRITE, 0, nullptr);
		kevent(kq, &change, 1, nullptr, 0, nullptr);
	}

	~CommandReader() {
		if (dir_fd >= 0) close(dir_fd);
		if (kq >= 0) close(kq);
	}

	Command poll() {
		Command cmd;

		// non-blocking check for directory changes
		struct kevent event;
		struct timespec timeout = {0, 0};
		int n = kevent(kq, nullptr, 0, &event, 1, &timeout);

		// even without a kqueue event, check if file exists
		// (handles the case where file was written before kqueue was set up)
		if (n <= 0) {
			struct stat st;
			if (stat(command_path.c_str(), &st) != 0)
				return cmd;
		}

		std::ifstream file(command_path);
		if (!file.is_open()) return cmd;

		std::string content((std::istreambuf_iterator<char>(file)),
			std::istreambuf_iterator<char>());
		file.close();
		std::remove(command_path.c_str());
		fprintf(stderr, "read command: %s\n", content.c_str());

		if (content.empty()) return cmd;

		auto get_string = [&](const std::string& key) -> std::string {
			std::string search = "\"" + key + "\":\"";
			auto pos = content.find(search);
			if (pos == std::string::npos) {
				search = "\"" + key + "\": \"";
				pos = content.find(search);
			}
			if (pos == std::string::npos) return "";
			pos += search.size();
			auto end = content.find("\"", pos);
			return content.substr(pos, end - pos);
		};

		auto get_double = [&](const std::string& key) -> double {
			std::string search = "\"" + key + "\":";
			auto pos = content.find(search);
			if (pos == std::string::npos) {
				search = "\"" + key + "\": ";
				pos = content.find(search);
			}
			if (pos == std::string::npos) return 0.0;
			pos += search.size();
			while (pos < content.size() && content[pos] == ' ') pos++;
			return std::stod(content.substr(pos));
		};

		std::string type = get_string("command");

		if (type == "load") {
			cmd.type = CommandType::Load;
			cmd.path = get_string("path");

			std::string focus = get_string("focus");
			if (!focus.empty()) {
				cmd.has_style = true;

				if (focus == "center") cmd.style.focus = FocusMethod::Center;
				else if (focus == "random") cmd.style.focus = FocusMethod::Random;
				else if (focus == "specific") cmd.style.focus = FocusMethod::Specific;
				else if (focus == "union") cmd.style.focus = FocusMethod::Union;

				std::string zoom_str = get_string("zoom");
				if (zoom_str == "fixed") cmd.style.zoom = ZoomMethod::Fixed;
				else if (zoom_str == "random") cmd.style.zoom = ZoomMethod::Random;
				else if (zoom_str == "fit") cmd.style.zoom = ZoomMethod::Fit;
				else if (zoom_str == "fit_points") cmd.style.zoom = ZoomMethod::FitPoints;

				std::string motion_str = get_string("motion");
				if (motion_str == "static") cmd.style.motion = MotionStyle::Static;
				else if (motion_str == "zoom_in") cmd.style.motion = MotionStyle::ZoomIn;
				else if (motion_str == "zoom_out") cmd.style.motion = MotionStyle::ZoomOut;
				else if (motion_str == "drift") cmd.style.motion = MotionStyle::Drift;
				else if (motion_str == "pan_to") cmd.style.motion = MotionStyle::PanTo;

				double zm = get_double("zoom_min");
				double zx = get_double("zoom_max");
				if (zm > 0.0) cmd.style.zoom_min = zm;
				if (zx > 0.0) cmd.style.zoom_max = zx;

				double drift = get_double("drift_magnitude");
				if (drift > 0.0) cmd.style.drift_magnitude = drift;

				double pad = get_double("padding");
				if (pad > 0.0) cmd.style.padding = pad;

				std::string points_str = get_string("points");
				if (!points_str.empty()) {
					std::istringstream pss(points_str);
					std::string pair;
					while (std::getline(pss, pair, ';')) {
						auto comma = pair.find(',');
						if (comma != std::string::npos) {
							double px = std::stod(pair.substr(0, comma));
							double py = std::stod(pair.substr(comma + 1));
							cmd.style.points.push_back({px, py});
						}
					}
				}
			} else {
				cmd.has_raw_keyframe = true;
				cmd.kf.start_x = get_double("start_x");
				cmd.kf.start_y = get_double("start_y");
				cmd.kf.start_zoom = get_double("start_zoom");
				cmd.kf.end_x = get_double("end_x");
				cmd.kf.end_y = get_double("end_y");
				cmd.kf.end_zoom = get_double("end_zoom");
			}
		} else if (type == "transition") {
			cmd.type = CommandType::Transition;
		} else if (type == "skip") {
			cmd.type = CommandType::Skip;
		} else if (type == "quit") {
			cmd.type = CommandType::Quit;
		} else if (type == "config") {
			cmd.type = CommandType::Config;
			cmd.config_key = get_string("key");
			cmd.config_value = get_double("value");
		}

		return cmd;

		return cmd;
	}
};

class StatusWriter {
	std::string status_path;
	std::string tmp_path;

public:
	StatusWriter(const std::string& dir) {
		status_path = dir + "/status.json";
		tmp_path = dir + "/status.json.tmp";
	}

	void write(Status& status) {
		std::ofstream file(tmp_path);
		if (!file.is_open()) return;

		file << "{"
			<< "\"phase\":\"" << status.phase << "\","
			<< "\"image\":\"" << status.image << "\","
			<< "\"progress\":" << status.progress << ","
			<< "\"last_key\":" << status.last_key << ","
			<< "\"frame\":" << status.frame << ","
			<< "\"preload_ready\":" << (status.preload_ready ? "true" : "false")
			<< "}" << std::endl;

		file.close();
		std::rename(tmp_path.c_str(), status_path.c_str());
	}
};

class EventWriter {
	std::string event_path;
	std::ofstream stream;

public:
	EventWriter(const std::string& dir) {
		event_path = dir + "/events.log";
		stream.open(event_path, std::ios::trunc);
		stream.flush();
	}

	void write_key(int keycode) {
		if (keycode < 0) return;
		stream << "key " << keycode << std::endl;
	}

	void write_phase(const std::string& phase) {
		stream << "phase " << phase << std::endl;
	}

	void write_event(const std::string& name) {
		stream << name << std::endl;
	}
};
