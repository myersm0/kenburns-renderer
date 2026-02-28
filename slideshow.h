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
	double fit_size = std::min(img_w / out_aspect, (double)img_h);
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

	ImagePyramid* current_pyramid = nullptr;
	Keyframe current_keyframe;
	int current_frame = 0;
	int total_frames = 1;

	ImagePyramid* next_pyramid = nullptr;
	Keyframe next_keyframe;
	std::string next_path;
	int transition_start_frame = 0;

	int hold_frames;
	int fade_frames;
	double dt;

	void compute_dt() {
		total_frames = hold_frames + fade_frames;
		dt = 1.0 / std::max(1, total_frames - 1);
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

	void start_transition() {
		if (phase != SlideshowPhase::Holding) return;
		if (!loader.ready()) return;

		next_pyramid = loader.collect();
		if (!next_pyramid) return;

		transition_start_frame = current_frame;
		phase = SlideshowPhase::Transitioning;
	}

	void skip() {
		if (phase == SlideshowPhase::Transitioning && next_pyramid) {
			if (current_pyramid) delete current_pyramid;
			current_pyramid = next_pyramid;
			next_pyramid = nullptr;
			current_keyframe = next_keyframe;
			current_frame = 0;
			phase = SlideshowPhase::Holding;
		} else if (phase == SlideshowPhase::Holding && loader.ready()) {
			ImagePyramid* incoming = loader.collect();
			if (incoming) {
				if (current_pyramid) delete current_pyramid;
				current_pyramid = incoming;
				current_keyframe = next_keyframe;
				current_frame = 0;
			}
		}
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

		if (phase == SlideshowPhase::Transitioning) {
			double fade_progress = (double)(current_frame - transition_start_frame) / fade_frames;
			fade_progress = std::min(fade_progress, 1.0);

			params.pyramid_b = next_pyramid;
			params.b_t = fade_progress * fade_frames / (double)total_frames;
			params.kf_b = next_keyframe;
			params.alpha = smoothstep(fade_progress);

			if (fade_progress >= 1.0) {
				if (current_pyramid) delete current_pyramid;
				current_pyramid = next_pyramid;
				next_pyramid = nullptr;
				current_keyframe = next_keyframe;
				current_frame = (int)(params.b_t * total_frames);
				phase = SlideshowPhase::Holding;
			}
		}

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
