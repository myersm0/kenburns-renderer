#pragma once
#include <string>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <sys/stat.h>
#include "slideshow.h"

#ifdef __APPLE__
#include <sys/event.h>
#endif
#include <fcntl.h>

// JSON subset accepted by this parser:
//   - flat objects only (no nesting)
//   - string values: "key":"value" or "key": "value"
//   - numeric values: "key":1.5 or "key": 1.5
//   - no escaped quotes within strings
//   - "points" is a semicolon-separated string: "0.3,0.4;0.7,0.6"
// This is sufficient because both producer and consumer are under our control.

enum class CommandType {
	None, Load, Transition, Swap, Cancel, Quit, Config
};

struct Command {
	CommandType type = CommandType::None;
	uint64_t seq = 0;
	std::string path;
	Keyframe kf;
	KeyframeParams style;
	bool has_raw_keyframe = false;
	bool has_style = false;
	std::string config_key;
	double config_value = 0.0;
};

namespace parse {

inline std::string get_string(const std::string& content, const std::string& key) {
	for (auto& sep : {"\":\"", "\": \""}) {
		std::string search = "\"" + key + sep;
		auto pos = content.find(search);
		if (pos != std::string::npos) {
			pos += search.size();
			auto end = content.find("\"", pos);
			if (end != std::string::npos)
				return content.substr(pos, end - pos);
		}
	}
	return "";
}

inline double get_double(const std::string& content, const std::string& key, double fallback = 0.0) {
	for (auto& sep : {"\":", "\": "}) {
		std::string search = "\"" + key + sep;
		auto pos = content.find(search);
		if (pos != std::string::npos) {
			pos += search.size();
			while (pos < content.size() && content[pos] == ' ') pos++;
			try {
				return std::stod(content.substr(pos));
			} catch (...) {
				return fallback;
			}
		}
	}
	return fallback;
}

inline FocusMethod focus_method(const std::string& s) {
	if (s == "random") return FocusMethod::Random;
	if (s == "specific") return FocusMethod::Specific;
	if (s == "union") return FocusMethod::Union;
	return FocusMethod::Center;
}

inline ZoomMethod zoom_method(const std::string& s) {
	if (s == "fixed") return ZoomMethod::Fixed;
	if (s == "fit") return ZoomMethod::Fit;
	if (s == "fit_points") return ZoomMethod::FitPoints;
	return ZoomMethod::Random;
}

inline MotionStyle motion_style(const std::string& s) {
	if (s == "zoom_in") return MotionStyle::ZoomIn;
	if (s == "zoom_out") return MotionStyle::ZoomOut;
	if (s == "drift") return MotionStyle::Drift;
	if (s == "pan_to") return MotionStyle::PanTo;
	return MotionStyle::Static;
}

inline std::vector<FocalPoint> points(const std::string& content) {
	std::vector<FocalPoint> result;
	std::string raw = get_string(content, "points");
	if (raw.empty()) return result;

	std::istringstream stream(raw);
	std::string pair;
	while (std::getline(stream, pair, ';')) {
		auto comma = pair.find(',');
		if (comma != std::string::npos) {
			try {
				double px = std::stod(pair.substr(0, comma));
				double py = std::stod(pair.substr(comma + 1));
				result.push_back({px, py});
			} catch (...) {}
		}
	}
	return result;
}

inline Command load(const std::string& content) {
	Command cmd;
	cmd.type = CommandType::Load;
	cmd.path = get_string(content, "path");

	std::string focus = get_string(content, "focus");
	if (!focus.empty()) {
		cmd.has_style = true;
		cmd.style.focus = focus_method(focus);
		cmd.style.zoom = zoom_method(get_string(content, "zoom"));
		cmd.style.motion = motion_style(get_string(content, "motion"));

		double zm = get_double(content, "zoom_min");
		double zx = get_double(content, "zoom_max");
		if (zm > 0.0) cmd.style.zoom_min = zm;
		if (zx > 0.0) cmd.style.zoom_max = zx;

		double drift = get_double(content, "drift_magnitude");
		if (drift > 0.0) cmd.style.drift_magnitude = drift;

		double pad = get_double(content, "padding");
		if (pad > 0.0) cmd.style.padding = pad;

		cmd.style.points = points(content);
	} else {
		cmd.has_raw_keyframe = true;
		cmd.kf.start_x = get_double(content, "start_x");
		cmd.kf.start_y = get_double(content, "start_y");
		cmd.kf.start_zoom = get_double(content, "start_zoom");
		cmd.kf.end_x = get_double(content, "end_x");
		cmd.kf.end_y = get_double(content, "end_y");
		cmd.kf.end_zoom = get_double(content, "end_zoom");

		std::string ctrl_test = get_string(content, "ctrl1_x");
		if (!ctrl_test.empty() || content.find("\"ctrl1_x\"") != std::string::npos) {
			cmd.kf.ctrl1_x = get_double(content, "ctrl1_x");
			cmd.kf.ctrl1_y = get_double(content, "ctrl1_y");
			cmd.kf.ctrl2_x = get_double(content, "ctrl2_x");
			cmd.kf.ctrl2_y = get_double(content, "ctrl2_y");
			cmd.kf.curved = true;
		}
	}

	return cmd;
}

} // namespace parse

class CommandReader {
	std::string command_path;
#ifdef __APPLE__
	int kq;
	int dir_fd;
#endif

public:
	CommandReader(const std::string& dir) {
		command_path = dir + "/command.json";

#ifdef __APPLE__
		kq = kqueue();
		dir_fd = open(dir.c_str(), O_RDONLY);

		struct kevent change;
		EV_SET(&change, dir_fd, EVFILT_VNODE,
			EV_ADD | EV_ENABLE | EV_CLEAR,
			NOTE_WRITE, 0, nullptr);
		kevent(kq, &change, 1, nullptr, 0, nullptr);
#endif
	}

	~CommandReader() {
#ifdef __APPLE__
		if (dir_fd >= 0) close(dir_fd);
		if (kq >= 0) close(kq);
#endif
	}

	Command poll() {
		Command cmd;

#ifdef __APPLE__
		struct kevent event;
		struct timespec timeout = {0, 0};
		int n = kevent(kq, nullptr, 0, &event, 1, &timeout);

		if (n <= 0) {
			struct stat st;
			if (stat(command_path.c_str(), &st) != 0)
				return cmd;
		}
#else
		struct stat st;
		if (stat(command_path.c_str(), &st) != 0)
			return cmd;
#endif

		std::ifstream file(command_path);
		if (!file.is_open()) return cmd;

		std::string content((std::istreambuf_iterator<char>(file)),
			std::istreambuf_iterator<char>());
		file.close();
		std::remove(command_path.c_str());

		if (content.empty()) return cmd;

		std::string type = parse::get_string(content, "command");

		if (type == "load") {
			cmd = parse::load(content);
		} else if (type == "transition") {
			cmd.type = CommandType::Transition;
		} else if (type == "swap") {
			cmd.type = CommandType::Swap;
		} else if (type == "cancel") {
			cmd.type = CommandType::Cancel;
		} else if (type == "quit") {
			cmd.type = CommandType::Quit;
		} else if (type == "config") {
			cmd.type = CommandType::Config;
			cmd.config_key = parse::get_string(content, "key");
			cmd.config_value = parse::get_double(content, "value");
		}

		cmd.seq = (uint64_t)parse::get_double(content, "seq");

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

	void write(const std::string& phase, bool preload_ready,
		bool fade_complete, bool paused,
		int source_w, int source_h,
		uint64_t last_seq, bool accepted,
		uint64_t preload_seq, uint64_t preload_failed_seq
	) {
		std::ofstream file(tmp_path);
		if (!file.is_open()) return;

		file << "{"
			<< "\"phase\":\"" << phase << "\","
			<< "\"preload_ready\":" << (preload_ready ? "true" : "false") << ","
			<< "\"fade_complete\":" << (fade_complete ? "true" : "false") << ","
			<< "\"paused\":" << (paused ? "true" : "false") << ","
			<< "\"source_w\":" << source_w << ","
			<< "\"source_h\":" << source_h << ","
			<< "\"last_seq\":" << last_seq << ","
			<< "\"accepted\":" << (accepted ? "true" : "false") << ","
			<< "\"preload_seq\":" << preload_seq << ","
			<< "\"preload_failed_seq\":" << preload_failed_seq
			<< "}" << std::endl;

		file.close();
		std::rename(tmp_path.c_str(), status_path.c_str());
	}
};

class KeyWriter {
	std::string path;
	std::ofstream stream;

public:
	KeyWriter(const std::string& dir) {
		path = dir + "/keys.log";
		stream.open(path, std::ios::trunc);
		stream.flush();
	}

	void write(int keycode) {
		if (keycode < 0) return;
		stream << keycode << std::endl;
	}
};
