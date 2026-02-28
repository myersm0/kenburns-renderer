#include <cstdio>
#include <cmath>
#include <cassert>
#include <cstdlib>
#include <ctime>

// need Keyframe definition before keyframe_builder.h
struct Keyframe {
	double start_x, start_y, start_zoom;
	double end_x, end_y, end_zoom;
};

#include "keyframe_builder.h"

int tests_run = 0;
int tests_passed = 0;

#define TEST(name) { \
	tests_run++; \
	printf("  %-50s", name); \
}
#define PASS { tests_passed++; printf("ok\n"); }
#define FAIL(msg) { printf("FAIL: %s\n", msg); }

#define NEAR(a, b, tol) (std::abs((a) - (b)) < (tol))
#define IN_RANGE(v, lo, hi) ((v) >= (lo) && (v) <= (hi))

// ---- clamp_center ----

void test_clamp_center_stays_centered() {
	TEST("clamp: centered point stays centered")
	auto r = clamp_center(0.5, 0.5, 1.0, 1000, 1000, 1920, 1080);
	if (NEAR(r.cx, 0.5, 0.001) && NEAR(r.cy, 0.5, 0.001)) PASS
	else FAIL("center moved")
}

void test_clamp_center_edge_clamped() {
	TEST("clamp: edge point gets pushed inward")
	auto r = clamp_center(0.05, 0.05, 1.5, 1000, 1000, 1920, 1080);
	if (r.cx > 0.05 && r.cy > 0.05) PASS
	else FAIL("point not clamped")
}

void test_clamp_center_low_zoom_centers() {
	TEST("clamp: low zoom forces center")
	auto r = clamp_center(0.2, 0.8, 0.5, 1000, 1000, 1920, 1080);
	if (NEAR(r.cx, 0.5, 0.01) && NEAR(r.cy, 0.5, 0.01)) PASS
	else FAIL("not centered")
}

void test_clamp_center_landscape_image() {
	TEST("clamp: wide image, high zoom")
	auto r = clamp_center(0.1, 0.5, 2.0, 4000, 2000, 1920, 1080);
	if (r.cx > 0.1) PASS
	else FAIL("x not clamped")
}

void test_clamp_center_portrait_image() {
	TEST("clamp: tall image, high zoom")
	auto r = clamp_center(0.5, 0.05, 2.0, 2000, 4000, 1920, 1080);
	if (r.cy > 0.05) PASS
	else FAIL("y not clamped")
}

void test_clamp_center_symmetric() {
	TEST("clamp: symmetric around center")
	auto lo = clamp_center(0.0, 0.0, 1.5, 1000, 1000, 1920, 1080);
	auto hi = clamp_center(1.0, 1.0, 1.5, 1000, 1000, 1920, 1080);
	if (NEAR(lo.cx, 1.0 - hi.cx, 0.001) && NEAR(lo.cy, 1.0 - hi.cy, 0.001)) PASS
	else FAIL("not symmetric")
}

// ---- compute_focus ----

void test_focus_center() {
	TEST("focus: center returns 0.5, 0.5")
	auto r = compute_focus(FocusMethod::Center, {});
	if (NEAR(r.cx, 0.5, 0.001) && NEAR(r.cy, 0.5, 0.001)) PASS
	else FAIL("not centered")
}

void test_focus_random_in_range() {
	TEST("focus: random stays in 0.3-0.7")
	bool ok = true;
	for (int i = 0; i < 100; i++) {
		auto r = compute_focus(FocusMethod::Random, {});
		if (!IN_RANGE(r.cx, 0.3, 0.7) || !IN_RANGE(r.cy, 0.3, 0.7)) {
			ok = false;
			break;
		}
	}
	if (ok) PASS
	else FAIL("out of range")
}

void test_focus_specific_single() {
	TEST("focus: specific uses first point")
	std::vector<FocalPoint> pts = {{0.3, 0.7}};
	auto r = compute_focus(FocusMethod::Specific, pts);
	if (NEAR(r.cx, 0.3, 0.001) && NEAR(r.cy, 0.7, 0.001)) PASS
	else FAIL("wrong point")
}

void test_focus_union_two_points() {
	TEST("focus: union of two points = midpoint")
	std::vector<FocalPoint> pts = {{0.2, 0.3}, {0.8, 0.7}};
	auto r = compute_focus(FocusMethod::Union, pts);
	if (NEAR(r.cx, 0.5, 0.001) && NEAR(r.cy, 0.5, 0.001)) PASS
	else FAIL("not midpoint")
}

void test_focus_union_three_points() {
	TEST("focus: union of three points = bbox center")
	std::vector<FocalPoint> pts = {{0.1, 0.2}, {0.9, 0.4}, {0.5, 0.8}};
	auto r = compute_focus(FocusMethod::Union, pts);
	// bbox: x=[0.1, 0.9] y=[0.2, 0.8], center = (0.5, 0.5)
	if (NEAR(r.cx, 0.5, 0.001) && NEAR(r.cy, 0.5, 0.001)) PASS
	else FAIL("wrong center")
}

void test_focus_union_single_point() {
	TEST("focus: union of one point = that point")
	std::vector<FocalPoint> pts = {{0.3, 0.7}};
	auto r = compute_focus(FocusMethod::Union, pts);
	if (NEAR(r.cx, 0.3, 0.001) && NEAR(r.cy, 0.7, 0.001)) PASS
	else FAIL("wrong point")
}

void test_focus_empty_points_fallback() {
	TEST("focus: empty points falls back to center")
	auto r = compute_focus(FocusMethod::Union, {});
	if (NEAR(r.cx, 0.5, 0.001) && NEAR(r.cy, 0.5, 0.001)) PASS
	else FAIL("not centered")
}

// ---- compute_zoom ----

void test_zoom_fixed() {
	TEST("zoom: fixed returns zoom_min")
	double z = compute_zoom(ZoomMethod::Fixed, 1.5, 1.5,
		{0.5, 0.5}, {}, 0.05, 1000, 1000, 1920, 1080);
	if (NEAR(z, 1.5, 0.001)) PASS
	else FAIL("wrong zoom")
}

void test_zoom_random_in_range() {
	TEST("zoom: random stays in range")
	bool ok = true;
	for (int i = 0; i < 100; i++) {
		double z = compute_zoom(ZoomMethod::Random, 0.8, 1.5,
			{0.5, 0.5}, {}, 0.05, 1000, 1000, 1920, 1080);
		if (!IN_RANGE(z, 0.8, 1.5)) { ok = false; break; }
	}
	if (ok) PASS
	else FAIL("out of range")
}

void test_zoom_fit_points_tight() {
	TEST("zoom: fit_points zooms to contain both points")
	std::vector<FocalPoint> pts = {{0.3, 0.4}, {0.7, 0.6}};
	double z = compute_zoom(ZoomMethod::FitPoints, 0, 0,
		{0.5, 0.5}, pts, 0.05, 1000, 1000, 1920, 1080);
	// span_x = 0.4 + 0.1 = 0.5, span_y = 0.2 + 0.1 = 0.3
	// zoom should be such that crop contains this span
	if (z > 0.5 && z < 5.0) PASS
	else { printf("z=%.3f ", z); FAIL("unexpected zoom") }
}

void test_zoom_fit_points_single() {
	TEST("zoom: fit_points single point = high zoom")
	std::vector<FocalPoint> pts = {{0.5, 0.5}};
	double z = compute_zoom(ZoomMethod::FitPoints, 0, 0,
		{0.5, 0.5}, pts, 0.05, 1000, 1000, 1920, 1080);
	// span = 0 + 0.1 padding, should zoom in quite a bit
	if (z > 2.0) PASS
	else { printf("z=%.3f ", z); FAIL("expected high zoom") }
}

void test_zoom_fit_points_wide_spread() {
	TEST("zoom: fit_points wide spread = low zoom")
	std::vector<FocalPoint> pts = {{0.1, 0.1}, {0.9, 0.9}};
	double z = compute_zoom(ZoomMethod::FitPoints, 0, 0,
		{0.5, 0.5}, pts, 0.05, 1000, 1000, 1920, 1080);
	if (z < 1.5) PASS
	else { printf("z=%.3f ", z); FAIL("expected low zoom") }
}

// ---- motion ----

void test_motion_static() {
	TEST("motion: static preserves focus and zoom")
	auto kf = apply_motion(MotionStyle::Static,
		{0.5, 0.5}, 1.2, 0.0, {},
		1000, 1000, 1920, 1080);
	if (NEAR(kf.start_zoom, 1.2, 0.001) && NEAR(kf.end_zoom, 1.2, 0.001)
		&& NEAR(kf.start_x, kf.end_x, 0.001)
		&& NEAR(kf.start_y, kf.end_y, 0.001)) PASS
	else FAIL("not static")
}

void test_motion_zoom_in() {
	TEST("motion: zoom_in end > start")
	auto kf = apply_motion(MotionStyle::ZoomIn,
		{0.5, 0.5}, 1.0, 0.0, {},
		1000, 1000, 1920, 1080);
	if (kf.end_zoom > kf.start_zoom) PASS
	else FAIL("zoom didn't increase")
}

void test_motion_zoom_out() {
	TEST("motion: zoom_out end < start")
	auto kf = apply_motion(MotionStyle::ZoomOut,
		{0.5, 0.5}, 1.0, 0.0, {},
		1000, 1000, 1920, 1080);
	if (kf.end_zoom < kf.start_zoom) PASS
	else FAIL("zoom didn't decrease")
}

void test_motion_drift_moves() {
	TEST("motion: drift produces movement")
	// run many times, at least some should move
	int moved = 0;
	for (int i = 0; i < 50; i++) {
		auto kf = apply_motion(MotionStyle::Drift,
			{0.5, 0.5}, 1.0, 0.15, {},
			1000, 1000, 1920, 1080);
		double dx = std::abs(kf.end_x - kf.start_x);
		double dy = std::abs(kf.end_y - kf.start_y);
		if (dx > 0.001 || dy > 0.001) moved++;
	}
	if (moved > 40) PASS
	else FAIL("too few moved")
}

void test_motion_pan_to() {
	TEST("motion: pan_to goes from first to second point")
	std::vector<FocalPoint> pts = {{0.3, 0.3}, {0.7, 0.7}};
	auto kf = apply_motion(MotionStyle::PanTo,
		{0.5, 0.5}, 2.0, 0.0, pts,
		1000, 1000, 1920, 1080);
	if (kf.start_x < kf.end_x && kf.start_y < kf.end_y) PASS
	else {
		printf("sx=%.3f ex=%.3f sy=%.3f ey=%.3f ",
			kf.start_x, kf.end_x, kf.start_y, kf.end_y);
		FAIL("not panning toward second point")
	}
}

// ---- build_keyframe integration ----

void test_build_center_static() {
	TEST("build: center + fixed + static")
	KeyframeParams params;
	params.focus = FocusMethod::Center;
	params.zoom = ZoomMethod::Fixed;
	params.motion = MotionStyle::Static;
	params.zoom_min = 1.0;
	auto kf = build_keyframe(params, 1000, 1000, 1920, 1080);
	if (NEAR(kf.start_x, 0.5, 0.05) && NEAR(kf.end_x, 0.5, 0.05)
		&& NEAR(kf.start_zoom, 1.0, 0.01)) PASS
	else FAIL("unexpected values")
}

void test_build_union_fitpoints_static() {
	TEST("build: union + fit_points + static")
	KeyframeParams params;
	params.focus = FocusMethod::Union;
	params.zoom = ZoomMethod::FitPoints;
	params.motion = MotionStyle::Static;
	params.points = {{0.3, 0.4}, {0.7, 0.6}};
	params.padding = 0.05;
	auto kf = build_keyframe(params, 1000, 1000, 1920, 1080);
	// center should be at midpoint of points
	if (NEAR(kf.start_x, 0.5, 0.1) && NEAR(kf.start_y, 0.5, 0.1)) PASS
	else FAIL("center wrong")
}

void test_build_result_always_clamped() {
	TEST("build: corner focus gets clamped")
	KeyframeParams params;
	params.focus = FocusMethod::Specific;
	params.zoom = ZoomMethod::Fixed;
	params.motion = MotionStyle::Static;
	params.zoom_min = 2.0;
	params.points = {{0.02, 0.02}};
	auto kf = build_keyframe(params, 1000, 1000, 1920, 1080);
	if (kf.start_x > 0.02 && kf.start_y > 0.02) PASS
	else FAIL("not clamped")
}

void test_build_union_pan() {
	TEST("build: pan between two focal points")
	KeyframeParams params;
	params.focus = FocusMethod::Center;  // ignored by PanTo
	params.zoom = ZoomMethod::Fixed;
	params.motion = MotionStyle::PanTo;
	params.zoom_min = 1.5;
	params.points = {{0.3, 0.3}, {0.7, 0.7}};
	auto kf = build_keyframe(params, 1000, 1000, 1920, 1080);
	if (kf.start_x < kf.end_x && kf.start_y < kf.end_y) PASS
	else FAIL("not panning")
}

// ---- main ----

int main() {
	srand((unsigned)time(nullptr));

	printf("clamp_center:\n");
	test_clamp_center_stays_centered();
	test_clamp_center_edge_clamped();
	test_clamp_center_low_zoom_centers();
	test_clamp_center_landscape_image();
	test_clamp_center_portrait_image();
	test_clamp_center_symmetric();

	printf("\ncompute_focus:\n");
	test_focus_center();
	test_focus_random_in_range();
	test_focus_specific_single();
	test_focus_union_two_points();
	test_focus_union_three_points();
	test_focus_union_single_point();
	test_focus_empty_points_fallback();

	printf("\ncompute_zoom:\n");
	test_zoom_fixed();
	test_zoom_random_in_range();
	test_zoom_fit_points_tight();
	test_zoom_fit_points_single();
	test_zoom_fit_points_wide_spread();

	printf("\nmotion:\n");
	test_motion_static();
	test_motion_zoom_in();
	test_motion_zoom_out();
	test_motion_drift_moves();
	test_motion_pan_to();

	printf("\nbuild_keyframe:\n");
	test_build_center_static();
	test_build_union_fitpoints_static();
	test_build_result_always_clamped();
	test_build_union_pan();

	printf("\n%d/%d passed\n", tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
