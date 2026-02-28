#include <opencv2/opencv.hpp>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <fstream>
#include <sstream>
#include <poll.h>
#include <unistd.h>

// ---- data types ----

struct crop_state {
	double center_x, center_y;
	double crop_w, crop_h;
};

struct image_pyramid {
	std::vector<cv::Mat> levels;
	int source_width, source_height;
};

struct keyframe {
	std::string path;
	double start_x, start_y, start_zoom;
	double end_x, end_y, end_zoom;
};

struct slideshow_config {
	int output_width = 3024;
	int output_height = 1964;
	int fps = 30;
	double hold_seconds = 5.0;
	double fade_seconds = 3.0;
	double blur_strength = 0.0;
	bool interactive = false;
};

// ---- helpers ----

double smoothstep(double t) {
	return t * t * (3.0 - 2.0 * t);
}

crop_state interpolate_crop(
	double t_raw,
	double start_x, double start_y, double start_zoom,
	double end_x, double end_y, double end_zoom,
	int img_w, int img_h,
	int output_width, int output_height
) {
	double t = smoothstep(t_raw);
	double zoom = start_zoom + (end_zoom - start_zoom) * t;

	double out_aspect = (double)output_width / output_height;
	double fit_size = std::min(img_w / out_aspect, (double)img_h);
	double crop_w = (fit_size * out_aspect) / zoom;
	double crop_h = fit_size / zoom;

	return {
		(start_x + (end_x - start_x) * t) * img_w,
		(start_y + (end_y - start_y) * t) * img_h,
		crop_w, crop_h
	};
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

image_pyramid* build_pyramid(cv::Mat& src) {
	image_pyramid* pyr = new image_pyramid();
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

void render_pyramid_frame(
	image_pyramid* pyr, crop_state& state,
	int output_width, int output_height,
	cv::Mat& affine, cv::Mat& dst
) {
	cv::Size out_size(output_width, output_height);
	double downsample_ratio = std::max(
		state.crop_w / output_width,
		state.crop_h / output_height
	);
	int level = (int)std::floor(std::log2(std::max(1.0, downsample_ratio)));
	level = std::min(level, (int)pyr->levels.size() - 1);

	double scale_x = pyr->levels[level].cols / (double)pyr->source_width;
	double scale_y = pyr->levels[level].rows / (double)pyr->source_height;

	affine.at<double>(0, 0) = (state.crop_w * scale_x) / output_width;
	affine.at<double>(0, 1) = 0.0;
	affine.at<double>(0, 2) = (state.center_x - state.crop_w * 0.5) * scale_x;
	affine.at<double>(1, 0) = 0.0;
	affine.at<double>(1, 1) = (state.crop_h * scale_y) / output_height;
	affine.at<double>(1, 2) = (state.center_y - state.crop_h * 0.5) * scale_y;

	cv::warpAffine(pyr->levels[level], dst, affine, out_size,
		cv::INTER_LINEAR | cv::WARP_INVERSE_MAP, cv::BORDER_CONSTANT,
		cv::Scalar(0, 0, 0));
}

// ---- preloader ----

struct preloader {
	std::thread worker;
	std::mutex mtx;
	std::condition_variable cv_ready;
	std::condition_variable cv_request;

	std::string requested_path;
	bool has_request = false;
	bool shutdown = false;

	cv::Mat loaded_image;
	image_pyramid* loaded_pyramid = nullptr;
	bool result_ready = false;

	void start() {
		worker = std::thread([this]() { run(); });
	}

	void run() {
		while (true) {
			std::unique_lock<std::mutex> lock(mtx);
			cv_request.wait(lock, [this]{ return has_request || shutdown; });
			if (shutdown) return;

			std::string path = requested_path;
			has_request = false;
			lock.unlock();

			cv::Mat img = cv::imread(path);
			image_pyramid* pyr = nullptr;
			if (!img.empty())
				pyr = build_pyramid(img);

			lock.lock();
			loaded_image = img;
			if (loaded_pyramid)
				delete loaded_pyramid;
			loaded_pyramid = pyr;
			result_ready = true;
			lock.unlock();
			cv_ready.notify_one();
		}
	}

	void request(const std::string& path) {
		std::lock_guard<std::mutex> lock(mtx);
		requested_path = path;
		has_request = true;
		result_ready = false;
		cv_request.notify_one();
	}

	image_pyramid* collect() {
		std::unique_lock<std::mutex> lock(mtx);
		cv_ready.wait(lock, [this]{ return result_ready; });
		result_ready = false;
		image_pyramid* pyr = loaded_pyramid;
		loaded_pyramid = nullptr;
		return pyr;
	}

	image_pyramid* try_collect() {
		std::lock_guard<std::mutex> lock(mtx);
		if (!result_ready) return nullptr;
		result_ready = false;
		image_pyramid* pyr = loaded_pyramid;
		loaded_pyramid = nullptr;
		return pyr;
	}

	void stop() {
		{
			std::lock_guard<std::mutex> lock(mtx);
			shutdown = true;
		}
		cv_request.notify_one();
		if (worker.joinable())
			worker.join();
		if (loaded_pyramid)
			delete loaded_pyramid;
	}
};

// ---- stdin helpers ----

bool stdin_has_data() {
	struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
	return poll(&pfd, 1, 0) > 0;
}

std::string read_stdin_line() {
	std::string line;
	std::getline(std::cin, line);
	return line;
}

// ---- playlist file parser ----

bool parse_playlist(
	const std::string& path,
	slideshow_config& config,
	std::vector<keyframe>& playlist
) {
	std::ifstream file(path);
	if (!file.is_open()) return false;

	std::string line;
	while (std::getline(file, line)) {
		if (line.empty() || line[0] == '#') continue;

		std::istringstream iss(line);
		std::string command;
		iss >> command;

		if (command == "config") {
			std::string key;
			iss >> key;
			if (key == "width") iss >> config.output_width;
			else if (key == "height") iss >> config.output_height;
			else if (key == "fps") iss >> config.fps;
			else if (key == "hold") iss >> config.hold_seconds;
			else if (key == "fade") iss >> config.fade_seconds;
			else if (key == "blur") iss >> config.blur_strength;
		} else if (command == "image") {
			keyframe kf;
			iss >> kf.path
				>> kf.start_x >> kf.start_y >> kf.start_zoom
				>> kf.end_x >> kf.end_y >> kf.end_zoom;
			playlist.push_back(kf);
		}
	}
	return !playlist.empty();
}

// ---- render one frame ----

int render_blended_frame(
	const char* window_name,
	image_pyramid* pyr_a, double a_t,
	double a_start_x, double a_start_y, double a_start_zoom,
	double a_end_x, double a_end_y, double a_end_zoom,
	image_pyramid* pyr_b, double b_t,
	double b_start_x, double b_start_y, double b_start_zoom,
	double b_end_x, double b_end_y, double b_end_zoom,
	double alpha, double dt, double blur_strength,
	int output_width, int output_height,
	cv::Mat& frame_a, cv::Mat& frame_b,
	cv::Mat& blended, cv::Mat& blurred, cv::Mat& affine,
	int wait_ms
) {
	crop_state sa = interpolate_crop(
		a_t, a_start_x, a_start_y, a_start_zoom,
		a_end_x, a_end_y, a_end_zoom,
		pyr_a->source_width, pyr_a->source_height,
		output_width, output_height
	);
	render_pyramid_frame(pyr_a, sa, output_width, output_height, affine, frame_a);

	cv::Mat* display = &frame_a;

	if (pyr_b && alpha > 0.0) {
		crop_state sb = interpolate_crop(
			b_t, b_start_x, b_start_y, b_start_zoom,
			b_end_x, b_end_y, b_end_zoom,
			pyr_b->source_width, pyr_b->source_height,
			output_width, output_height
		);
		render_pyramid_frame(pyr_b, sb, output_width, output_height, affine, frame_b);
		cv::addWeighted(frame_a, 1.0 - alpha, frame_b, alpha, 0.0, blended);
		display = &blended;
	}

	if (blur_strength > 0.0 && dt > 0.0) {
		crop_state prev = interpolate_crop(
			std::max(0.0, a_t - dt),
			a_start_x, a_start_y, a_start_zoom,
			a_end_x, a_end_y, a_end_zoom,
			pyr_a->source_width, pyr_a->source_height,
			output_width, output_height
		);
		crop_state next = interpolate_crop(
			std::min(1.0, a_t + dt),
			a_start_x, a_start_y, a_start_zoom,
			a_end_x, a_end_y, a_end_zoom,
			pyr_a->source_width, pyr_a->source_height,
			output_width, output_height
		);
		double dx = (next.center_x - prev.center_x) * 0.5 * output_width / sa.crop_w;
		double dy = (next.center_y - prev.center_y) * 0.5 * output_height / sa.crop_h;
		cv::Mat kernel = make_motion_kernel(dx * blur_strength, dy * blur_strength);

		if (!kernel.empty()) {
			cv::filter2D(*display, blurred, -1, kernel);
			display = &blurred;
		}
	}

	cv::imshow(window_name, *display);
	return cv::waitKey(wait_ms);
}

// ---- standalone playlist mode ----

void run_playlist(slideshow_config& config, std::vector<keyframe>& playlist) {
	const char* window_name = "slideshow";
	cv::namedWindow(window_name, cv::WINDOW_NORMAL);
	cv::moveWindow(window_name, 0, 0);
	cv::setWindowProperty(window_name, cv::WND_PROP_FULLSCREEN, cv::WINDOW_FULLSCREEN);

	cv::Size out_size(config.output_width, config.output_height);
	cv::Mat frame_a(out_size, CV_8UC3);
	cv::Mat frame_b(out_size, CV_8UC3);
	cv::Mat blended_mat(out_size, CV_8UC3);
	cv::Mat blurred_mat(out_size, CV_8UC3);
	cv::Mat affine(2, 3, CV_64F);

	int wait_ms = 1000 / config.fps;
	int hold_frames = (int)(config.hold_seconds * config.fps);
	int fade_frames = (int)(config.fade_seconds * config.fps);
	int total_frames = hold_frames + fade_frames;
	double dt = 1.0 / std::max(1, total_frames - 1);

	preloader loader;
	loader.start();

	cv::Mat first_img = cv::imread(playlist[0].path);
	if (first_img.empty()) {
		fprintf(stderr, "failed to load %s\n", playlist[0].path.c_str());
		loader.stop();
		return;
	}

	image_pyramid* current_pyr = build_pyramid(first_img);
	keyframe* current_kf = &playlist[0];
	int current_frame = 0;
	int image_index = 0;

	image_pyramid* next_pyr = nullptr;
	bool preload_started = false;

	if (playlist.size() > 1)  {
		loader.request(playlist[1].path);
		preload_started = true;
	}

	bool running = true;
	while (running) {
		double a_t = std::min((double)current_frame / total_frames, 1.0);

		bool in_transition = current_frame >= hold_frames
			&& image_index < (int)playlist.size() - 1;

		if (in_transition && !next_pyr) {
			next_pyr = loader.collect();
			if (!next_pyr) {
				fprintf(stderr, "failed to load %s\n",
					playlist[image_index + 1].path.c_str());
				running = false;
				break;
			}
		}

		int keypress;
		if (in_transition) {
			keyframe* next_kf = &playlist[image_index + 1];
			double fade_progress = (double)(current_frame - hold_frames) / fade_frames;
			double alpha = smoothstep(std::min(fade_progress, 1.0));
			double b_t = fade_progress * fade_frames / (double)total_frames;

			keypress = render_blended_frame(
				window_name,
				current_pyr, a_t,
				current_kf->start_x, current_kf->start_y, current_kf->start_zoom,
				current_kf->end_x, current_kf->end_y, current_kf->end_zoom,
				next_pyr, b_t,
				next_kf->start_x, next_kf->start_y, next_kf->start_zoom,
				next_kf->end_x, next_kf->end_y, next_kf->end_zoom,
				alpha, dt, config.blur_strength,
				config.output_width, config.output_height,
				frame_a, frame_b, blended_mat, blurred_mat, affine,
				wait_ms
			);

			if (fade_progress >= 1.0) {
				delete current_pyr;
				current_pyr = next_pyr;
				next_pyr = nullptr;
				image_index++;
				current_kf = &playlist[image_index];
				current_frame = (int)(b_t * total_frames);
				preload_started = false;

				if (image_index < (int)playlist.size() - 1) {
					loader.request(playlist[image_index + 1].path);
					preload_started = true;
				}
			}
		} else {
			keypress = render_blended_frame(
				window_name,
				current_pyr, a_t,
				current_kf->start_x, current_kf->start_y, current_kf->start_zoom,
				current_kf->end_x, current_kf->end_y, current_kf->end_zoom,
				nullptr, 0.0,
				0, 0, 0, 0, 0, 0,
				0.0, dt, config.blur_strength,
				config.output_width, config.output_height,
				frame_a, frame_b, blended_mat, blurred_mat, affine,
				wait_ms
			);

			if (!preload_started && image_index < (int)playlist.size() - 1) {
				loader.request(playlist[image_index + 1].path);
				preload_started = true;
			}
		}

		current_frame++;

		if (current_frame > total_frames && image_index >= (int)playlist.size() - 1)
			running = false;

		if (keypress == 27)
			running = false;
	}

	delete current_pyr;
	if (next_pyr) delete next_pyr;
	loader.stop();
	cv::destroyAllWindows();
}

// ---- interactive IPC mode ----

void run_interactive(slideshow_config& config) {
	const char* window_name = "slideshow";
	cv::namedWindow(window_name, cv::WINDOW_NORMAL);
	cv::moveWindow(window_name, 0, 0);
	cv::setWindowProperty(window_name, cv::WND_PROP_FULLSCREEN, cv::WINDOW_FULLSCREEN);

	cv::Size out_size(config.output_width, config.output_height);
	cv::Mat frame_a(out_size, CV_8UC3);
	cv::Mat frame_b(out_size, CV_8UC3);
	cv::Mat blended_mat(out_size, CV_8UC3);
	cv::Mat blurred_mat(out_size, CV_8UC3);
	cv::Mat affine(2, 3, CV_64F);

	int wait_ms = 1000 / config.fps;
	int hold_frames = (int)(config.hold_seconds * config.fps);
	int fade_frames = (int)(config.fade_seconds * config.fps);
	int total_frames = hold_frames + fade_frames;
	double dt = 1.0 / std::max(1, total_frames - 1);

	preloader loader;
	loader.start();

	image_pyramid* current_pyr = nullptr;
	image_pyramid* next_pyr = nullptr;
	keyframe current_kf, next_kf;
	int current_frame = 0;
	bool has_current = false;
	bool transitioning = false;
	bool preload_pending = false;

	printf("ready\n");
	fflush(stdout);

	bool running = true;
	while (running) {
		while (stdin_has_data()) {
			std::string line = read_stdin_line();
			if (line.empty()) continue;

			std::istringstream iss(line);
			std::string command;
			iss >> command;

			if (command == "quit") {
				running = false;
				break;
			} else if (command == "image") {
				keyframe kf;
				iss >> kf.path
					>> kf.start_x >> kf.start_y >> kf.start_zoom
					>> kf.end_x >> kf.end_y >> kf.end_zoom;

				if (!has_current) {
					cv::Mat img = cv::imread(kf.path);
					if (!img.empty()) {
						current_pyr = build_pyramid(img);
						current_kf = kf;
						current_frame = 0;
						has_current = true;
						printf("showing %s\n", kf.path.c_str());
						fflush(stdout);
					} else {
						printf("error failed to load %s\n", kf.path.c_str());
						fflush(stdout);
					}
				} else {
					next_kf = kf;
					loader.request(kf.path);
					preload_pending = true;
					printf("loading %s\n", kf.path.c_str());
					fflush(stdout);
				}
			} else if (command == "next") {
				if (has_current && preload_pending && !transitioning) {
					next_pyr = loader.collect();
					if (next_pyr) {
						transitioning = true;
						preload_pending = false;
						printf("transitioning %s\n", next_kf.path.c_str());
						fflush(stdout);
					}
				}
			} else if (command == "config") {
				std::string key;
				iss >> key;
				if (key == "width") iss >> config.output_width;
				else if (key == "height") iss >> config.output_height;
				else if (key == "fps") { iss >> config.fps; wait_ms = 1000 / config.fps; }
				else if (key == "hold") {
					iss >> config.hold_seconds;
					hold_frames = (int)(config.hold_seconds * config.fps);
					total_frames = hold_frames + fade_frames;
					dt = 1.0 / std::max(1, total_frames - 1);
				}
				else if (key == "fade") {
					iss >> config.fade_seconds;
					fade_frames = (int)(config.fade_seconds * config.fps);
					total_frames = hold_frames + fade_frames;
					dt = 1.0 / std::max(1, total_frames - 1);
				}
				else if (key == "blur") iss >> config.blur_strength;
			}
		}

		if (!has_current) {
			cv::waitKey(wait_ms);
			continue;
		}

		double a_t = std::min((double)current_frame / total_frames, 1.0);

		if (transitioning) {
			double fade_progress = (double)(current_frame - hold_frames) / fade_frames;
			fade_progress = std::max(0.0, std::min(1.0, fade_progress));
			double alpha = smoothstep(fade_progress);
			double b_t = fade_progress * fade_frames / (double)total_frames;

			int keypress = render_blended_frame(
				window_name,
				current_pyr, a_t,
				current_kf.start_x, current_kf.start_y, current_kf.start_zoom,
				current_kf.end_x, current_kf.end_y, current_kf.end_zoom,
				next_pyr, b_t,
				next_kf.start_x, next_kf.start_y, next_kf.start_zoom,
				next_kf.end_x, next_kf.end_y, next_kf.end_zoom,
				alpha, dt, config.blur_strength,
				config.output_width, config.output_height,
				frame_a, frame_b, blended_mat, blurred_mat, affine,
				wait_ms
			);

			if (keypress >= 0) {
				printf("key %d\n", keypress);
				fflush(stdout);
			}

			if (fade_progress >= 1.0) {
				delete current_pyr;
				current_pyr = next_pyr;
				next_pyr = nullptr;
				current_kf = next_kf;
				current_frame = (int)(b_t * total_frames);
				transitioning = false;
				printf("done\n");
				fflush(stdout);
			}
		} else {
			int keypress = render_blended_frame(
				window_name,
				current_pyr, a_t,
				current_kf.start_x, current_kf.start_y, current_kf.start_zoom,
				current_kf.end_x, current_kf.end_y, current_kf.end_zoom,
				nullptr, 0.0,
				0, 0, 0, 0, 0, 0,
				0.0, dt, config.blur_strength,
				config.output_width, config.output_height,
				frame_a, frame_b, blended_mat, blurred_mat, affine,
				wait_ms
			);

			if (keypress >= 0) {
				printf("key %d\n", keypress);
				fflush(stdout);
			}
		}

		current_frame++;
	}

	if (current_pyr) delete current_pyr;
	if (next_pyr) delete next_pyr;
	loader.stop();
	cv::destroyAllWindows();
}

// ---- main ----

void print_usage() {
	printf("usage:\n");
	printf("  ken_burns_slideshow <playlist.txt>\n");
	printf("  ken_burns_slideshow --interactive [--width W] [--height H] [--fps F]\n");
	printf("\n");
	printf("playlist format:\n");
	printf("  # comment\n");
	printf("  config width 1920\n");
	printf("  config height 1080\n");
	printf("  config fps 30\n");
	printf("  config hold 5.0\n");
	printf("  config fade 3.0\n");
	printf("  config blur 0.0\n");
	printf("  image /path/to/img.jpg sx sy sz ex ey ez\n");
	printf("\n");
	printf("interactive commands (stdin):\n");
	printf("  image <path> <sx> <sy> <sz> <ex> <ey> <ez>\n");
	printf("  next\n");
	printf("  config <key> <value>\n");
	printf("  quit\n");
}

int main(int argc, char** argv) {
	if (argc < 2) {
		print_usage();
		return 1;
	}

	slideshow_config config;

	if (std::string(argv[1]) == "--interactive") {
		for (int i = 2; i < argc - 1; i += 2) {
			std::string flag = argv[i];
			if (flag == "--width") config.output_width = atoi(argv[i + 1]);
			else if (flag == "--height") config.output_height = atoi(argv[i + 1]);
			else if (flag == "--fps") config.fps = atoi(argv[i + 1]);
			else if (flag == "--hold") config.hold_seconds = atof(argv[i + 1]);
			else if (flag == "--fade") config.fade_seconds = atof(argv[i + 1]);
			else if (flag == "--blur") config.blur_strength = atof(argv[i + 1]);
		}
		run_interactive(config);
	} else {
		std::vector<keyframe> playlist;
		if (!parse_playlist(argv[1], config, playlist)) {
			fprintf(stderr, "failed to parse playlist %s\n", argv[1]);
			return 1;
		}
		run_playlist(config, playlist);
	}

	return 0;
}
