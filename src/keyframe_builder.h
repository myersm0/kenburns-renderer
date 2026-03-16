#pragma once
#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

// re-uses Keyframe from slideshow.h

enum class FocusMethod { Center, Random, Specific, Union };
enum class ZoomMethod { Fixed, Random, Fit, FitPoints };
enum class MotionStyle { Static, ZoomIn, ZoomOut, Drift, PanTo };

struct FocalPoint {
	double x, y;  // normalized 0-1
};

struct CenterPair {
	double cx, cy;
};

struct KeyframeParams {
	FocusMethod focus = FocusMethod::Center;
	ZoomMethod zoom = ZoomMethod::Random;
	MotionStyle motion = MotionStyle::Drift;
	double zoom_min = 0.9;
	double zoom_max = 1.3;
	double drift_magnitude = 0.1;
	double padding = 0.05;  // fraction of image to pad around focal region
	std::vector<FocalPoint> points;
};

// ---- helpers ----

inline std::mt19937& rng() {
	thread_local std::mt19937 engine{std::random_device{}()};
	return engine;
}

inline double random_double(double lo, double hi) {
	std::uniform_real_distribution<double> dist(lo, hi);
	return dist(rng());
}

inline CenterPair clamp_center(
	double cx, double cy, double zoom,
	int image_width, int image_height,
	int output_width, int output_height
) {
	double out_aspect = (double)output_width / output_height;
	double fit_size = std::max(image_width / out_aspect, (double)image_height);
	double crop_w = (fit_size * out_aspect) / zoom;
	double crop_h = fit_size / zoom;

	double margin_x = crop_w / (2.0 * image_width);
	double margin_y = crop_h / (2.0 * image_height);

	CenterPair result;
	if (crop_w <= image_width)
		result.cx = std::clamp(cx, margin_x, 1.0 - margin_x);
	else
		result.cx = 0.5;

	if (crop_h <= image_height)
		result.cy = std::clamp(cy, margin_y, 1.0 - margin_y);
	else
		result.cy = 0.5;

	return result;
}

inline Keyframe clamp_keyframe(
	Keyframe kf,
	int image_width, int image_height,
	int output_width, int output_height
) {
	auto s = clamp_center(kf.start_x, kf.start_y, kf.start_zoom,
		image_width, image_height, output_width, output_height);
	auto e = clamp_center(kf.end_x, kf.end_y, kf.end_zoom,
		image_width, image_height, output_width, output_height);

	Keyframe result = kf;
	result.start_x = s.cx;
	result.start_y = s.cy;
	result.end_x = e.cx;
	result.end_y = e.cy;

	if (kf.curved) {
		double min_zoom = std::min(kf.start_zoom, kf.end_zoom);
		auto c1 = clamp_center(kf.ctrl1_x, kf.ctrl1_y, min_zoom,
			image_width, image_height, output_width, output_height);
		auto c2 = clamp_center(kf.ctrl2_x, kf.ctrl2_y, min_zoom,
			image_width, image_height, output_width, output_height);
		result.ctrl1_x = c1.cx;
		result.ctrl1_y = c1.cy;
		result.ctrl2_x = c2.cx;
		result.ctrl2_y = c2.cy;
	}

	return result;
}

// ---- focal point computation ----

inline CenterPair compute_focus(
	FocusMethod method,
	const std::vector<FocalPoint>& points
) {
	switch (method) {
	case FocusMethod::Center:
		return {0.5, 0.5};

	case FocusMethod::Random:
		return {
			random_double(0.3, 0.7),
			random_double(0.3, 0.7)
		};

	case FocusMethod::Specific:
		if (points.empty()) return {0.5, 0.5};
		return {points[0].x, points[0].y};

	case FocusMethod::Union:
		if (points.empty()) return {0.5, 0.5};
		{
			double min_x = points[0].x, max_x = points[0].x;
			double min_y = points[0].y, max_y = points[0].y;
			for (auto& p : points) {
				min_x = std::min(min_x, p.x);
				max_x = std::max(max_x, p.x);
				min_y = std::min(min_y, p.y);
				max_y = std::max(max_y, p.y);
			}
			return {
				(min_x + max_x) * 0.5,
				(min_y + max_y) * 0.5
			};
		}
	}
	return {0.5, 0.5};
}

// ---- zoom computation ----

inline double compute_zoom(
	ZoomMethod method,
	double zoom_min, double zoom_max,
	CenterPair focus,
	const std::vector<FocalPoint>& points,
	double padding,
	int image_width, int image_height,
	int output_width, int output_height
) {
	switch (method) {
	case ZoomMethod::Fixed:
		return zoom_min;

	case ZoomMethod::Random:
		return random_double(zoom_min, zoom_max);

	case ZoomMethod::Fit: {
		double out_aspect = (double)output_width / output_height;
		double fit_size = std::max(image_width / out_aspect, (double)image_height);
		return std::max(fit_size * out_aspect / image_width, fit_size / (double)image_height);
	}

	case ZoomMethod::FitPoints: {
		if (points.empty()) return 1.0;
		double min_x = points[0].x, max_x = points[0].x;
		double min_y = points[0].y, max_y = points[0].y;
		for (auto& p : points) {
			min_x = std::min(min_x, p.x);
			max_x = std::max(max_x, p.x);
			min_y = std::min(min_y, p.y);
			max_y = std::max(max_y, p.y);
		}
		double span_x = (max_x - min_x) + 2.0 * padding;
		double span_y = (max_y - min_y) + 2.0 * padding;

		double out_aspect = (double)output_width / output_height;
		double fit_size = std::max(image_width / out_aspect, (double)image_height);
		double needed_w = span_x * image_width;
		double needed_h = span_y * image_height;

		double zoom_for_w = (fit_size * out_aspect) / needed_w;
		double zoom_for_h = fit_size / needed_h;
		double zoom = std::min(zoom_for_w, zoom_for_h);

		return std::max(zoom, 0.1);
	}
	}
	return 1.0;
}

// ---- motion computation ----

inline Keyframe apply_motion(
	MotionStyle motion,
	CenterPair focus, double zoom,
	double drift_magnitude,
	const std::vector<FocalPoint>& points,
	int image_width, int image_height,
	int output_width, int output_height
) {
	Keyframe kf;

	switch (motion) {
	case MotionStyle::Static:
		kf = {focus.cx, focus.cy, zoom, focus.cx, focus.cy, zoom};
		break;

	case MotionStyle::ZoomIn: {
		double start_zoom = zoom * 0.85;
		double end_zoom = zoom * 1.15;
		kf = {focus.cx, focus.cy, start_zoom, focus.cx, focus.cy, end_zoom};
		break;
	}

	case MotionStyle::ZoomOut: {
		double start_zoom = zoom * 1.15;
		double end_zoom = zoom * 0.85;
		kf = {focus.cx, focus.cy, start_zoom, focus.cx, focus.cy, end_zoom};
		break;
	}

	case MotionStyle::Drift: {
		double dx = random_double(-drift_magnitude, drift_magnitude);
		double dy = random_double(-drift_magnitude, drift_magnitude);
		double dz = random_double(-0.1, 0.1) * zoom;

		double sx = focus.cx - dx * 0.5;
		double sy = focus.cy - dy * 0.5;
		double ex = focus.cx + dx * 0.5;
		double ey = focus.cy + dy * 0.5;

		double perp_x = -(ey - sy);
		double perp_y = ex - sx;
		double arc = random_double(-0.4, 0.4);

		kf.start_x = sx;
		kf.start_y = sy;
		kf.start_zoom = zoom - dz * 0.5;
		kf.end_x = ex;
		kf.end_y = ey;
		kf.end_zoom = zoom + dz * 0.5;
		kf.ctrl1_x = sx + (ex - sx) / 3.0 + perp_x * arc;
		kf.ctrl1_y = sy + (ey - sy) / 3.0 + perp_y * arc;
		kf.ctrl2_x = sx + 2.0 * (ex - sx) / 3.0 + perp_x * arc;
		kf.ctrl2_y = sy + 2.0 * (ey - sy) / 3.0 + perp_y * arc;
		kf.curved = true;
		break;
	}

	case MotionStyle::PanTo: {
		if (points.size() < 2) {
			kf.start_x = focus.cx;
			kf.start_y = focus.cy;
			kf.start_zoom = zoom;
			kf.end_x = focus.cx;
			kf.end_y = focus.cy;
			kf.end_zoom = zoom;
		} else {
			double out_aspect = (double)output_width / output_height;
			double fit_size = std::max(image_width / out_aspect, (double)image_height);

			double min_zoom = zoom;
			for (auto& p : {points[0], points[1]}) {
				double slack_x = std::min(p.x, 1.0 - p.x);
				double slack_y = std::min(p.y, 1.0 - p.y);
				if (slack_x > 0.0)
					min_zoom = std::max(min_zoom,
						(fit_size * out_aspect) / (2.0 * image_width * slack_x));
				if (slack_y > 0.0)
					min_zoom = std::max(min_zoom,
						fit_size / (2.0 * image_height * slack_y));
			}

			double sx = points[0].x, sy = points[0].y;
			double ex = points[1].x, ey = points[1].y;
			double perp_x = -(ey - sy);
			double perp_y = ex - sx;
			double arc = random_double(0.15, 0.35);
			if (random_double(0.0, 1.0) < 0.5) arc = -arc;

			kf.start_x = sx;
			kf.start_y = sy;
			kf.start_zoom = min_zoom;
			kf.end_x = ex;
			kf.end_y = ey;
			kf.end_zoom = min_zoom;
			kf.ctrl1_x = sx + (ex - sx) / 3.0 + perp_x * arc;
			kf.ctrl1_y = sy + (ey - sy) / 3.0 + perp_y * arc;
			kf.ctrl2_x = sx + 2.0 * (ex - sx) / 3.0 - perp_x * arc;
			kf.ctrl2_y = sy + 2.0 * (ey - sy) / 3.0 - perp_y * arc;
			kf.curved = true;
		}
		break;
	}
	}

	return clamp_keyframe(kf, image_width, image_height, output_width, output_height);
}

// ---- main entry point ----

inline Keyframe build_keyframe(
	KeyframeParams& params,
	int image_width, int image_height,
	int output_width, int output_height
) {
	CenterPair focus = compute_focus(params.focus, params.points);

	double zoom = compute_zoom(
		params.zoom, params.zoom_min, params.zoom_max,
		focus, params.points, params.padding,
		image_width, image_height,
		output_width, output_height
	);

	return apply_motion(
		params.motion, focus, zoom,
		params.drift_magnitude, params.points,
		image_width, image_height,
		output_width, output_height
	);
}
