#pragma once
#include <opencv2/opencv.hpp>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>

// ---- data types ----

struct CropState {
	double center_x, center_y;
	double crop_w, crop_h;
};

struct Keyframe {
	double start_x, start_y, start_zoom;
	double end_x, end_y, end_zoom;
};

struct ImagePyramid {
	std::vector<cv::Mat> levels;
	int source_width, source_height;
};

enum class SlideshowPhase { Idle, Holding, Transitioning };

struct RenderParams {
	ImagePyramid* pyramid_a = nullptr;
	double a_t = 0.0;
	Keyframe kf_a;

	ImagePyramid* pyramid_b = nullptr;
	double b_t = 0.0;
	Keyframe kf_b;

	double alpha = 0.0;
	double dt = 0.0;
	bool valid = false;

	std::vector<std::pair<double,double>> debug_points;
	Keyframe debug_keyframe;
	int debug_source_w = 0;
	int debug_source_h = 0;
};

// ---- pure helpers ----

inline double smoothstep(double t) {
	return t * t * (3.0 - 2.0 * t);
}

inline CropState interpolate_crop(
	double t_raw, Keyframe& kf,
	int img_w, int img_h,
	int output_width, int output_height
) {
	double t = smoothstep(t_raw);
	double zoom = kf.start_zoom + (kf.end_zoom - kf.start_zoom) * t;

	double out_aspect = (double)output_width / output_height;
	double fit_size = std::max(img_w / out_aspect, (double)img_h);
	double crop_w = (fit_size * out_aspect) / zoom;
	double crop_h = fit_size / zoom;

	return {
		(kf.start_x + (kf.end_x - kf.start_x) * t) * img_w,
		(kf.start_y + (kf.end_y - kf.start_y) * t) * img_h,
		crop_w, crop_h
	};
}

inline ImagePyramid* build_pyramid(cv::Mat& src) {
	ImagePyramid* pyr = new ImagePyramid();
	pyr->source_width = src.cols;
	pyr->source_height = src.rows;
	pyr->levels.push_back(src);
	while (pyr->levels.back().cols > 64 && pyr->levels.back().rows > 64) {
		cv::Mat half;
		cv::pyrDown(pyr->levels.back(), half);
		pyr->levels.push_back(half);
	}
	return pyr;
}

cv::Mat make_motion_kernel(double dx, double dy) {
	double length = std::sqrt(dx * dx + dy * dy);
	if (length < 1.0) return cv::Mat();

	int size = ((int)std::round(length)) | 1;
	int mid = size / 2;

	cv::Mat kernel = cv::Mat::zeros(size, size, CV_32F);
	for (int k = 0; k < size; k++)
		kernel.at<float>(mid, k) = 1.0f;

	double angle = std::atan2(dy, dx) * 180.0 / CV_PI;
	cv::Mat rotation = cv::getRotationMatrix2D(cv::Point2f(mid, mid), -angle, 1.0);
	cv::Mat rotated;
	cv::warpAffine(kernel, rotated, rotation, kernel.size(), cv::INTER_LINEAR);

	double sum = cv::sum(rotated)[0];
	if (sum > 0) rotated /= sum;
	return rotated;
}

#include "keyframe_builder.h"

// ---- preloader ----

class Preloader {
	std::thread worker;
	std::mutex mtx;
	std::condition_variable request_cv;

	std::string requested_path;
	bool has_request = false;
	std::atomic<bool> shutdown{false};

	cv::Mat loaded_image;
	ImagePyramid* loaded_pyramid = nullptr;
	std::atomic<bool> result_ready{false};

	void run() {
		while (!shutdown) {
			std::unique_lock<std::mutex> lock(mtx);
			request_cv.wait(lock, [this]{ return has_request || shutdown.load(); });
			if (shutdown) return;

			std::string path = requested_path;
			has_request = false;
			lock.unlock();

			cv::Mat img = cv::imread(path);
			ImagePyramid* pyr = nullptr;
			if (!img.empty())
				pyr = build_pyramid(img);

			std::lock_guard<std::mutex> guard(mtx);
			if (loaded_pyramid)
				delete loaded_pyramid;
			loaded_image = img;
			loaded_pyramid = pyr;
			result_ready = true;
		}
	}

public:
	void start() { worker = std::thread([this]{ run(); }); }

	void request(const std::string& path) {
		std::lock_guard<std::mutex> lock(mtx);
		requested_path = path;
		has_request = true;
		result_ready = false;
		request_cv.notify_one();
	}

	bool ready() { return result_ready; }

	ImagePyramid* collect() {
		std::lock_guard<std::mutex> lock(mtx);
		if (!result_ready) return nullptr;
		result_ready = false;
		ImagePyramid* pyr = loaded_pyramid;
		loaded_pyramid = nullptr;
		return pyr;
	}

	void stop() {
		shutdown = true;
		request_cv.notify_one();
		if (worker.joinable())
			worker.join();
		std::lock_guard<std::mutex> lock(mtx);
		if (loaded_pyramid)
			delete loaded_pyramid;
		loaded_pyramid = nullptr;
	}
};

// ---- state machine ----

class SlideshowState {
	SlideshowPhase phase = SlideshowPhase::Idle;
	Preloader loader;
	KeyframeParams pending_style;
	bool has_pending_style = false;

	ImagePyramid* current_pyramid = nullptr;
	Keyframe current_keyframe;
	std::vector<std::pair<double,double>> current_points;
	int current_frame = 0;
	int total_frames = 1;

	ImagePyramid* next_pyramid = nullptr;
	Keyframe next_keyframe;
	std::vector<std::pair<double,double>> next_points;
	std::string next_path;
	int transition_start_frame = 0;

	int hold_frames;
	int fade_frames;
	double dt;
	bool fade_complete_flag = false;

	void compute_dt() {
		total_frames = hold_frames + fade_frames;
		dt = 1.0 / std::max(1, total_frames - 1);
	}

	auto extract_points(KeyframeParams& style) {
		std::vector<std::pair<double,double>> pts;
		for (auto& p : style.points)
			pts.push_back({p.x, p.y});
		return pts;
	}

public:
	int output_width = 1920;
	int output_height = 1080;
	double blur_strength = 0.0;

	SlideshowState(int fps, double hold_seconds, double fade_seconds) {
		hold_frames = (int)(hold_seconds * fps);
		fade_frames = (int)(fade_seconds * fps);
		compute_dt();
		loader.start();
	}

	~SlideshowState() {
		loader.stop();
		if (current_pyramid) delete current_pyramid;
		if (next_pyramid) delete next_pyramid;
	}

	SlideshowPhase get_phase() { return phase; }
	bool preload_ready() { return loader.ready(); }
	bool fade_complete() { return fade_complete_flag; }

	void load(const std::string& path, Keyframe kf) {
		if (phase == SlideshowPhase::Idle) {
			cv::Mat img = cv::imread(path);
			if (img.empty()) return;
			current_pyramid = build_pyramid(img);
			current_keyframe = kf;
			current_frame = 0;
			phase = SlideshowPhase::Holding;
		} else {
			next_keyframe = kf;
			next_path = path;
			loader.request(path);
		}
	}

	void load_with_style(const std::string& path, KeyframeParams& style) {
		if (phase == SlideshowPhase::Idle) {
			cv::Mat img = cv::imread(path);
			if (img.empty()) return;
			current_pyramid = build_pyramid(img);
			current_keyframe = build_keyframe(style,
				img.cols, img.rows, output_width, output_height);
			current_points = extract_points(style);
			current_frame = 0;
			phase = SlideshowPhase::Holding;
		} else {
			pending_style = style;
			has_pending_style = true;
			next_path = path;
			loader.request(path);
		}
	}

	bool start_transition() {
		if (phase != SlideshowPhase::Holding) return false;
		if (!loader.ready()) return false;

		next_pyramid = loader.collect();
		if (!next_pyramid) return false;

		if (has_pending_style) {
			next_keyframe = build_keyframe(pending_style,
				next_pyramid->source_width, next_pyramid->source_height,
				output_width, output_height);
			next_points = extract_points(pending_style);
			has_pending_style = false;
		}

		transition_start_frame = current_frame;
		fade_complete_flag = false;
		phase = SlideshowPhase::Transitioning;
		return true;
	}

	bool swap() {
		if (phase == SlideshowPhase::Transitioning && next_pyramid) {
			if (current_pyramid) delete current_pyramid;
			current_pyramid = next_pyramid;
			next_pyramid = nullptr;
			current_keyframe = next_keyframe;
			current_points = next_points;
			current_frame = 0;
			fade_complete_flag = false;
			phase = SlideshowPhase::Holding;
			return true;
		}
		if (phase == SlideshowPhase::Holding && loader.ready()) {
			ImagePyramid* incoming = loader.collect();
			if (!incoming) return false;
			if (has_pending_style) {
				next_keyframe = build_keyframe(pending_style,
					incoming->source_width, incoming->source_height,
					output_width, output_height);
				next_points = extract_points(pending_style);
				has_pending_style = false;
			}
			if (current_pyramid) delete current_pyramid;
			current_pyramid = incoming;
			current_keyframe = next_keyframe;
			current_points = next_points;
			current_frame = 0;
			return true;
		}
		return false;
	}

	bool cancel_transition() {
		if (phase != SlideshowPhase::Transitioning) return false;
		if (next_pyramid) {
			delete next_pyramid;
			next_pyramid = nullptr;
		}
		fade_complete_flag = false;
		phase = SlideshowPhase::Holding;
		return true;
	}

	RenderParams tick() {
		RenderParams params;

		if (phase == SlideshowPhase::Idle) return params;

		double a_t = std::min((double)current_frame / total_frames, 1.0);

		params.pyramid_a = current_pyramid;
		params.a_t = a_t;
		params.kf_a = current_keyframe;
		params.dt = dt;
		params.valid = true;
		params.debug_points = current_points;
		params.debug_keyframe = current_keyframe;
		params.debug_source_w = current_pyramid->source_width;
		params.debug_source_h = current_pyramid->source_height;

		if (phase == SlideshowPhase::Transitioning) {
			double fade_progress = (double)(current_frame - transition_start_frame) / fade_frames;
			fade_progress = std::min(fade_progress, 1.0);

			params.pyramid_b = next_pyramid;
			params.b_t = fade_progress * fade_frames / (double)total_frames;
			params.kf_b = next_keyframe;
			params.alpha = smoothstep(fade_progress);

			if (fade_progress >= 1.0) {
				fade_complete_flag = true;
				params.debug_points = next_points;
				params.debug_keyframe = next_keyframe;
				params.debug_source_w = next_pyramid->source_width;
				params.debug_source_h = next_pyramid->source_height;
			}
		}

		if (!fade_complete_flag)
			current_frame++;

		return params;
	}

	void set_hold(double seconds, int fps) {
		hold_frames = (int)(seconds * fps);
		compute_dt();
	}

	void set_fade(double seconds, int fps) {
		fade_frames = (int)(seconds * fps);
		compute_dt();
	}
};
