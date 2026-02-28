#pragma once
#include "slideshow.h"

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
		double blur_strength, int wait_ms
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

		cv::imshow(window_name, *display);
		return cv::waitKey(wait_ms);
	}
};
