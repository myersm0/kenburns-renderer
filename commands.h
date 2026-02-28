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
			cmd.kf.start_x = get_double("start_x");
			cmd.kf.start_y = get_double("start_y");
			cmd.kf.start_zoom = get_double("start_zoom");
			cmd.kf.end_x = get_double("end_x");
			cmd.kf.end_y = get_double("end_y");
			cmd.kf.end_zoom = get_double("end_zoom");
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
