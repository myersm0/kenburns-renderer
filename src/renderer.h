#pragma once
#include "slideshow.h"
#include <cstdio>

class Renderer {
	cv::Size out_size;
	cv::Mat frame_a, frame_b, blended, blurred, affine;

	void render_pyramid_frame(
		ImagePyramid* pyr, CropState& state, cv::Mat& dst
	) {
		double downsample_ratio = std::max(
			state.crop_w / out_size.width,
			state.crop_h / out_size.height
		);
		int level = (int)std::floor(std::log2(std::max(1.0, downsample_ratio)));
		level = std::min(level, (int)pyr->levels.size() - 1);

		double scale_x = pyr->levels[level].cols / (double)pyr->source_width;
		double scale_y = pyr->levels[level].rows / (double)pyr->source_height;

		affine.at<double>(0, 0) = (state.crop_w * scale_x) / out_size.width;
		affine.at<double>(0, 1) = 0.0;
		affine.at<double>(0, 2) = (state.center_x - state.crop_w * 0.5) * scale_x;
		affine.at<double>(1, 0) = 0.0;
		affine.at<double>(1, 1) = (state.crop_h * scale_y) / out_size.height;
		affine.at<double>(1, 2) = (state.center_y - state.crop_h * 0.5) * scale_y;

		cv::warpAffine(pyr->levels[level], dst, affine, out_size,
			cv::INTER_LINEAR | cv::WARP_INVERSE_MAP, cv::BORDER_CONSTANT,
			cv::Scalar(0, 0, 0));
	}

	cv::Point image_to_screen(
		double norm_x, double norm_y, CropState& crop,
		int source_w, int source_h
	) {
		double img_x = norm_x * source_w;
		double img_y = norm_y * source_h;
		double sx = (img_x - (crop.center_x - crop.crop_w * 0.5))
			* out_size.width / crop.crop_w;
		double sy = (img_y - (crop.center_y - crop.crop_h * 0.5))
			* out_size.height / crop.crop_h;
		return cv::Point((int)sx, (int)sy);
	}

	void draw_debug(cv::Mat& display, RenderParams& params, CropState& crop) {
		int src_w = params.debug_source_w;
		int src_h = params.debug_source_h;
		if (src_w == 0 || src_h == 0) return;

		double fs = out_size.height / 1080.0;
		int thick_bg = std::max(3, (int)(3 * fs));
		int thick_fg = std::max(1, (int)(1 * fs));
		int y = (int)(40 * fs);
		int line_h = (int)(30 * fs);

		Keyframe& kf = params.debug_keyframe;

		// image boundary
		cv::Point tl = image_to_screen(0.0, 0.0, crop, src_w, src_h);
		cv::Point br = image_to_screen(1.0, 1.0, crop, src_w, src_h);
		cv::rectangle(display, tl, br, cv::Scalar(80, 80, 80), 1);

		// focal points — red filled with white outline
		for (auto& p : params.debug_points) {
			cv::Point pt = image_to_screen(p.first, p.second, crop, src_w, src_h);
			cv::circle(display, pt, 10, cv::Scalar(255, 255, 255), 2);
			cv::circle(display, pt, 8, cv::Scalar(0, 0, 255), -1);
		}

		// bounding box of focal points — yellow dashed (solid approx)
		if (params.debug_points.size() >= 2) {
			double min_x = 1.0, max_x = 0.0, min_y = 1.0, max_y = 0.0;
			for (auto& p : params.debug_points) {
				min_x = std::min(min_x, p.first);
				max_x = std::max(max_x, p.first);
				min_y = std::min(min_y, p.second);
				max_y = std::max(max_y, p.second);
			}
			cv::Point bb_tl = image_to_screen(min_x, min_y, crop, src_w, src_h);
			cv::Point bb_br = image_to_screen(max_x, max_y, crop, src_w, src_h);
			cv::rectangle(display, bb_tl, bb_br, cv::Scalar(0, 255, 255), 1);
		}

		// start position — green circle
		cv::Point start = image_to_screen(kf.start_x, kf.start_y, crop, src_w, src_h);
		cv::drawMarker(display, start, cv::Scalar(0, 255, 0),
			cv::MARKER_CROSS, 20, 2);

		// end position — blue circle
		cv::Point end = image_to_screen(kf.end_x, kf.end_y, crop, src_w, src_h);
		cv::drawMarker(display, end, cv::Scalar(255, 100, 0),
			cv::MARKER_TILTED_CROSS, 20, 2);

		// motion path
		if (kf.curved) {
			const int segments = 32;
			for (int i = 0; i < segments; i++) {
				double t0 = (double)i / segments;
				double t1 = (double)(i + 1) / segments;
				auto b = [&](double t, double p0, double p1, double p2, double p3) {
					double u = 1.0 - t;
					return u*u*u*p0 + 3.0*u*u*t*p1 + 3.0*u*t*t*p2 + t*t*t*p3;
				};
				cv::Point a_pt = image_to_screen(
					b(t0, kf.start_x, kf.ctrl1_x, kf.ctrl2_x, kf.end_x),
					b(t0, kf.start_y, kf.ctrl1_y, kf.ctrl2_y, kf.end_y),
					crop, src_w, src_h);
				cv::Point b_pt = image_to_screen(
					b(t1, kf.start_x, kf.ctrl1_x, kf.ctrl2_x, kf.end_x),
					b(t1, kf.start_y, kf.ctrl1_y, kf.ctrl2_y, kf.end_y),
					crop, src_w, src_h);
				cv::line(display, a_pt, b_pt, cv::Scalar(200, 200, 200), 1);
			}
			// control points — small cyan diamonds
			cv::Point c1 = image_to_screen(kf.ctrl1_x, kf.ctrl1_y, crop, src_w, src_h);
			cv::Point c2 = image_to_screen(kf.ctrl2_x, kf.ctrl2_y, crop, src_w, src_h);
			cv::drawMarker(display, c1, cv::Scalar(255, 255, 0),
				cv::MARKER_DIAMOND, 12, 1);
			cv::drawMarker(display, c2, cv::Scalar(255, 255, 0),
				cv::MARKER_DIAMOND, 12, 1);
		} else {
			cv::line(display, start, end, cv::Scalar(200, 200, 200), 1);
		}

		// current center — white crosshair
		int cx = out_size.width / 2;
		int cy = out_size.height / 2;
		cv::drawMarker(display, cv::Point(cx, cy), cv::Scalar(255, 255, 255),
			cv::MARKER_CROSS, 40, 1);

		// text overlay — zoom, progress, keyframe info
		double current_zoom = kf.start_zoom
			+ (kf.end_zoom - kf.start_zoom) * smoothstep(params.a_t);
		char buf[256];
		snprintf(buf, sizeof(buf), "zoom: %.2f  t: %.2f", current_zoom, params.a_t);
		cv::putText(display, buf, cv::Point(20, y),
			 cv::FONT_HERSHEY_SIMPLEX, 0.7 * fs, cv::Scalar(0, 0, 0), thick_bg);
		cv::putText(display, buf, cv::Point(20, y),
			 cv::FONT_HERSHEY_SIMPLEX, 0.7 * fs, cv::Scalar(255, 255, 255), thick_fg);
		y += line_h;

		snprintf(buf, sizeof(buf), "kf: (%.2f,%.2f,%.2f) -> (%.2f,%.2f,%.2f)",
			kf.start_x, kf.start_y, kf.start_zoom,
			kf.end_x, kf.end_y, kf.end_zoom);
		cv::putText(display, buf, cv::Point(20, y),
			 cv::FONT_HERSHEY_SIMPLEX, 0.7 * fs, cv::Scalar(0, 0, 0), thick_bg);
		cv::putText(display, buf, cv::Point(20, y),
			 cv::FONT_HERSHEY_SIMPLEX, 0.7 * fs, cv::Scalar(255, 255, 255), thick_fg);
		y += line_h;

		snprintf(buf, sizeof(buf), "src: %dx%d  points: %d",
			src_w, src_h, (int)params.debug_points.size());
		cv::putText(display, buf, cv::Point(20, y),
			 cv::FONT_HERSHEY_SIMPLEX, 0.7 * fs, cv::Scalar(0, 0, 0), thick_bg);
		cv::putText(display, buf, cv::Point(20, y),
			 cv::FONT_HERSHEY_SIMPLEX, 0.7 * fs, cv::Scalar(255, 255, 255), thick_fg);
		y += line_h;
	}

public:
	Renderer(int width, int height)
		: out_size(width, height)
		, frame_a(cv::Size(width, height), CV_8UC3)
		, frame_b(cv::Size(width, height), CV_8UC3)
		, blended(cv::Size(width, height), CV_8UC3)
		, blurred(cv::Size(width, height), CV_8UC3)
		, affine(2, 3, CV_64F)
	{}

	int render(const char* window_name, RenderParams& params,
		double blur_strength, int wait_ms, bool debug = false
	) {
		if (!params.valid) return cv::waitKey(wait_ms);

		CropState sa = interpolate_crop(
			params.a_t, params.kf_a,
			params.pyramid_a->source_width,
			params.pyramid_a->source_height,
			out_size.width, out_size.height
		);
		render_pyramid_frame(params.pyramid_a, sa, frame_a);

		cv::Mat* display = &frame_a;

		if (params.pyramid_b && params.alpha > 0.0) {
			CropState sb = interpolate_crop(
				params.b_t, params.kf_b,
				params.pyramid_b->source_width,
				params.pyramid_b->source_height,
				out_size.width, out_size.height
			);
			render_pyramid_frame(params.pyramid_b, sb, frame_b);
			cv::addWeighted(frame_a, 1.0 - params.alpha,
				frame_b, params.alpha, 0.0, blended);
			display = &blended;
		}

		if (blur_strength > 0.0 && params.dt > 0.0) {
			CropState prev = interpolate_crop(
				std::max(0.0, params.a_t - params.dt), params.kf_a,
				params.pyramid_a->source_width,
				params.pyramid_a->source_height,
				out_size.width, out_size.height
			);
			CropState next = interpolate_crop(
				std::min(1.0, params.a_t + params.dt), params.kf_a,
				params.pyramid_a->source_width,
				params.pyramid_a->source_height,
				out_size.width, out_size.height
			);
			double dx = (next.center_x - prev.center_x) * 0.5
				* out_size.width / sa.crop_w;
			double dy = (next.center_y - prev.center_y) * 0.5
				* out_size.height / sa.crop_h;
			cv::Mat kernel = make_motion_kernel(
				dx * blur_strength, dy * blur_strength);

			if (!kernel.empty()) {
				cv::filter2D(*display, blurred, -1, kernel);
				display = &blurred;
			}
		}

		if (debug)
			draw_debug(*display, params, sa);

		cv::imshow(window_name, *display);
		return cv::waitKey(wait_ms);
	}
};
