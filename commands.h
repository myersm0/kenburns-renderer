#pragma once
#include <string>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <sys/stat.h>
#include "slideshow.h"

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
	time_t last_mtime = 0;

public:
	CommandReader(const std::string& dir) {
		command_path = dir + "/command.json";
	}

	Command poll() {
		Command cmd;

		struct stat st;
		if (stat(command_path.c_str(), &st) != 0) return cmd;
		if (st.st_mtime == last_mtime) return cmd;
		last_mtime = st.st_mtime;

		std::ifstream file(command_path);
		if (!file.is_open()) return cmd;

		std::string content((std::istreambuf_iterator<char>(file)),
			std::istreambuf_iterator<char>());
		file.close();
		std::remove(command_path.c_str());

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
