# kenburns-renderer

A standalone C++ Ken Burns effect renderer with mipmap-based anti-aliasing, Bézier curved motion paths, directional motion blur, smooth crossfade transitions, and background image preloading. Designed to be controlled by an external process via file-based IPC.

## Building

Requires OpenCV 4.x with highgui, imgproc, and imgcodecs. C++17.

```bash
g++ -o kbr src/main.cpp -Isrc \
    -I/path/to/opencv/include/opencv4 \
    -L/path/to/opencv/lib \
    -Wl,-rpath,/path/to/opencv/lib \
    -lopencv_core -lopencv_imgproc -lopencv_highgui -lopencv_imgcodecs \
    -std=c++17 -pthread -O2
```

Or with CMake:

```bash
mkdir build && cd build
cmake .. && make
```

## Quick start

Generate some test images and run the demo controller:

```bash
python demo/generate_images.py
julia demo/demo.jl
```

This cycles through three images with different motion styles (drift with curved arc, pan_to with S-curve, zoom_in), crossfading between them. Press `?` in the renderer window to see the debug overlay with the Bézier curve visualization.

## Usage

```bash
./kbr <command_dir> [options]
```

| Flag        | Default | Description                                      |
|-------------|---------|--------------------------------------------------|
| `--width`   | 1920    | Output resolution width                          |
| `--height`  | 1080    | Output resolution height                         |
| `--fps`     | 30      | Target frame rate                                |
| `--hold`    | 5.0     | Seconds to hold each image before transitioning  |
| `--fade`    | 3.0     | Crossfade duration in seconds                    |
| `--timeout` | 300.0   | Seconds of inactivity before auto-quit           |
| `--output`  |         | Write frames instead of displaying (see below)   |

The program opens a fullscreen OpenCV window and enters its render loop. It reads commands from `<command_dir>/command.json` and writes status to `<command_dir>/status.json`.

The idle timeout is refreshed by both IPC commands and keypresses.

## Headless output

With `--output`, kbr skips the window and writes frames directly. The controller drives it via IPC as usual and sends `quit` when done.

**Pipe to ffmpeg** (raw BGR frames to stdout):
```bash
./kbr /tmp/kbr_cmd --output - --fps 30 | \
  ffmpeg -f rawvideo -pix_fmt bgr24 -s 1920x1080 -r 30 -i - \
  -c:v libx264 -pix_fmt yuv420p output.mp4
```

**Numbered PNGs** (for debugging or frame-level tools):
```bash
./kbr /tmp/kbr_cmd --output /tmp/frames/
```

Frames are written as `frame_000001.png`, `frame_000002.png`, etc.

In headless mode there is no keyboard input — all control is via the IPC protocol. Frames are rendered as fast as possible with no frame-rate pacing. The status file and idle timeout still work normally.

## Keyboard controls

| Key        | Action                                            |
|------------|---------------------------------------------------|
| Spacebar   | Toggle pause (freezes animation on current frame) |
| `?`        | Toggle debug overlay                              |
| `q` or ESC | Quit immediately                                 |

Spacebar, `?`, `q`, and ESC are consumed by the renderer. All other keypresses are written to `<command_dir>/keys.log` for the controller to handle.

## Debug overlay

Press `?` during playback to toggle the debug overlay. Combine with spacebar pause to freeze a frame and inspect the geometry. The overlay draws:

| Element                | Color        | Meaning                                              |
|------------------------|--------------|------------------------------------------------------|
| Thin rectangle         | Gray         | Image boundary in output space. Visible when the crop extends beyond the image. |
| Filled circles         | Red / white  | Focal points from the `points` field in the load command. |
| Rectangle around dots  | Yellow       | Bounding box of focal points (when 2+ present). |
| Cross marker           | Green        | Start position of the animation. |
| Tilted cross marker    | Blue         | End position of the animation. |
| Curve or line          | Gray         | Motion path — Bézier curve for drift/pan_to, straight line for others. |
| Diamond markers        | Cyan         | Bézier control points (only shown for curved paths). |
| Crosshair at center    | White        | Screen center (current crop center). |

**Text readout** (top-left corner): current zoom and animation progress, the raw keyframe coordinates, and source image dimensions.

## Zoom semantics

Zoom uses the camera/film convention. 1.0 means the entire image is visible within the output frame (with letterboxing/pillarboxing if aspect ratios differ). 2.0 means half the linear extent is visible (2× magnification). Values below 1.0 show the full image with increasing black borders.

The reference frame is the largest output-aspect-ratio rectangle containing the entire source image, making zoom values portable across resolutions.

## Curved motion paths

The `drift` and `pan_to` motion styles produce cubic Bézier curves rather than straight lines. This gives the camera a natural, cinematographic feel — gentle arcs for drift, S-curves for pan_to.

A Bézier path is defined by four points in normalized image coordinates: the start and end positions (from the keyframe) plus two control points that shape the curve. When control points coincide with the endpoints (or when `curved` is false), the path is just a straight line.

For raw keyframe commands, you can specify control points explicitly:

```json
{
    "command": "load",
    "path": "/path/to/image.jpg",
    "start_x": 0.3, "start_y": 0.3, "start_zoom": 1.5,
    "end_x": 0.7, "end_y": 0.7, "end_zoom": 1.5,
    "ctrl1_x": 0.2, "ctrl1_y": 0.6,
    "ctrl2_x": 0.8, "ctrl2_y": 0.4
}
```

If control points are omitted, a straight line is used.

## Architecture

Five source files in `src/`, one compilation unit:

| File                 | Purpose                                                    |
|----------------------|------------------------------------------------------------|
| `slideshow.h`        | Types, Bézier interpolation, mipmap builder, state machine, preloader thread |
| `keyframe_builder.h` | Focal point, zoom, motion, and curve computation           |
| `renderer.h`         | Pyramid-aware rendering with motion blur and debug overlay |
| `commands.h`         | File-based command/status/key I/O                          |
| `main.cpp`           | Main loop wiring everything together                       |

### Render pipeline

Each frame follows this path:

1. State machine `tick()` produces a `RenderParams` struct describing what to draw
2. Renderer computes a `CropState` via `interpolate_crop` with smoothstep easing and Bézier path evaluation
3. Selects the appropriate mipmap level (`log2` of downsample ratio) to avoid aliasing
4. Builds an affine matrix mapping output pixels back to the pyramid level
5. `warpAffine` with `BORDER_CONSTANT` fills black for any out-of-bounds regions
6. If crossfading, renders both images and blends with `addWeighted`
7. If motion blur is enabled, computes a directional kernel from the frame-to-frame velocity and applies `filter2D`
8. If debug mode is active, draws the overlay on top of the final frame

### Mipmap pyramid

Each image is decomposed into a Gaussian pyramid via `pyrDown`. Per frame, the renderer selects the level whose resolution is closest to 1:1 with the output crop, eliminating aliasing artifacts on fine detail (foliage, hair, fabric textures). Memory overhead is 1.33× the source image. The preloader thread builds the next image's pyramid in the background during the hold phase.

### Preloader

The preloader thread services one request at a time, and every request carries the sequence number of the `load` command that issued it. When a worker finishes, it compares its own sequence against the current request before publishing: a result superseded by a newer request is discarded rather than stored. Readiness is therefore always attributable to a specific load — a stale pyramid from a superseded request can never be collected by a later `transition` or `swap`.

### State machine

`SlideshowState` manages three phases: Idle (waiting for the first image), Holding (displaying an image with Ken Burns animation), and Transitioning (crossfading between current and next image). Transitions are explicitly triggered via the `transition` command. The state machine never auto-advances — the controlling process decides timing.

A `load` arriving while an image is displayed buffers its keyframe (raw or style-derived) alongside the preload request; nothing touches the live animation until the incoming pyramid is collected by a `transition` or `swap`. Loading during a crossfade is therefore safe and does not disturb the fade in progress.

## IPC Protocol

The protocol is sequence-acknowledged: every command carries a `seq`, and the status file reports which sequence was last processed and whether it was accepted. Controllers should keep at most one command in flight — send, wait for acknowledgment, then send the next. This makes acknowledgments edges rather than levels, and prevents the single command slot from being overwritten before kbr reads it.

### Commands (controller → kbr)

Write JSON to `<command_dir>/command.json` via atomic rename from a `.tmp` file. The renderer deletes the file after reading.

The JSON parser accepts flat objects with string and numeric values, no nesting. No escaped quotes within strings. This is sufficient because both producer and consumer are under your control.

Every command may carry a `seq` field: a positive integer, monotonically increasing per session. If omitted, kbr assigns the previous sequence plus one, so controllers predating this protocol still produce distinguishable acknowledgments — but cannot correlate them, and should be updated.

**Load with style (kbr computes keyframe):**

```json
{
    "command": "load",
    "seq": 12,
    "path": "/absolute/path/to/image.jpg",
    "focus": "random",
    "zoom": "random",
    "motion": "drift",
    "zoom_min": 0.9,
    "zoom_max": 1.3,
    "drift_magnitude": 0.15,
    "padding": 0.05,
    "points": "0.3,0.4;0.7,0.6"
}
```

**Load with raw keyframe (controller computes keyframe):**

```json
{
    "command": "load",
    "seq": 13,
    "path": "/absolute/path/to/image.jpg",
    "start_x": 0.5, "start_y": 0.5, "start_zoom": 1.0,
    "end_x": 0.4, "end_y": 0.4, "end_zoom": 1.5
}
```

Optionally include `ctrl1_x`, `ctrl1_y`, `ctrl2_x`, `ctrl2_y` for a curved path.

**Other commands:**

```json
{"command": "transition", "seq": 14}
{"command": "swap", "seq": 15}
{"command": "cancel", "seq": 16}
{"command": "quit", "seq": 17}
{"command": "config", "seq": 18, "key": "blur", "value": 0.3}
```

`transition` starts a crossfade to the preloaded image. `swap` immediately replaces the current image (either completing a transition or cutting without a fade). `cancel` aborts a transition in progress and returns to holding. Config keys: `blur` (motion blur strength), `hold` (hold duration in seconds), `fade` (fade duration in seconds).

Commands are legal only in certain states: `transition` requires Holding with a ready preload, `swap` requires Transitioning or Holding-with-ready-preload, `cancel` requires Transitioning. An illegal command is rejected — reported via `accepted`, never silently dropped.

### Status (kbr → controller)

Written on state changes to `<command_dir>/status.json` (atomic rename):

```json
{
    "phase": "holding",
    "preload_ready": true,
    "fade_complete": false,
    "paused": false,
    "source_w": 2400,
    "source_h": 1600,
    "last_seq": 12,
    "accepted": true,
    "preload_seq": 12,
    "preload_failed_seq": 0
}
```

`last_seq` is the sequence of the most recently processed command, and `accepted` whether it was legal in the state that received it. A controller waits for `last_seq` to reach its command's sequence, then reads `accepted` to learn the outcome. A rejected command leaves state unchanged; the controller decides whether to retry, wait, or take another path.

`preload_seq` is the sequence of the `load` whose pyramid is currently ready, or 0 if none is. Controllers should test `preload_seq == my_load_seq` rather than the bare `preload_ready` boolean: the boolean cannot distinguish a preload for the image you just requested from one for an image you have since superseded. `preload_failed_seq` reports a load whose image could not be read, so a bad path surfaces as an immediate error rather than a wait that never ends.

`source_w` and `source_h` are the pixel dimensions of the currently displayed image. They are 0 before any image is loaded. This is useful for controllers that run external analysis and need to convert pixel coordinates to the normalized [0,1] coordinates that the `points` field expects: `point_x = pixel_x / source_w`.

### Keypresses (kbr → controller)

Non-consumed keypresses are appended to `<command_dir>/keys.log`, one keycode per line.

## Keyframe Builder

When a `load` command includes `focus`/`zoom`/`motion` fields, kbr computes the keyframe after loading the image (so it knows the image dimensions). All coordinates are normalized to [0, 1] relative to image dimensions.

### Focus methods

| Value      | Behavior                                                |
|------------|---------------------------------------------------------|
| `center`   | Focus on image center (0.5, 0.5)                       |
| `random`   | Random point in the central region (0.3–0.7)           |
| `specific` | Use the first point from `points`                      |
| `union`    | Center of the bounding box of all `points`             |

### Zoom methods

| Value        | Behavior                                              |
|--------------|-------------------------------------------------------|
| `fixed`      | Use `zoom_min` as the zoom level                      |
| `random`     | Random value between `zoom_min` and `zoom_max`        |
| `fit`        | Zoom to fill the output without borders               |
| `fit_points` | Zoom to contain all `points` with `padding`           |

Note that `points` carries bare coordinates, so `union` and `fit_points` operate on the extent of the point set. Controllers wanting to frame a region should send its corners as two points rather than its center as one.

### Motion styles

| Value      | Behavior                                                |
|------------|---------------------------------------------------------|
| `static`   | No movement — still frame (linear)                     |
| `zoom_in`  | Start at 85% zoom, end at 115% (linear)               |
| `zoom_out` | Start at 115% zoom, end at 85% (linear)               |
| `drift`    | Random gentle pan and zoom shift along a curved arc    |
| `pan_to`   | S-curve from first point to second point in `points`   |

`drift` generates a C-shaped arc by offsetting control points perpendicular to the motion direction. `pan_to` generates an S-curve with control points deflected in opposite directions, giving a natural "look at this, now look at that" camera feel. Both compute a minimum zoom that keeps both endpoints visible, so the pan direction is never squashed by clamping.

The `zoom_in` and `zoom_out` amplitudes are fixed at ±15% and are not adjustable from the load command; controllers needing a gentler or stronger zoom must send a raw keyframe instead.

All keyframes (including control points) are automatically clamped to avoid unnecessary black borders.

### Extending with new methods

1. Add the enum value in `keyframe_builder.h`
2. Add the corresponding case in `compute_focus`, `compute_zoom`, or `apply_motion`
3. Add the string mapping in the `parse` namespace in `commands.h`
4. Add a test case in `test/test_keyframe.cpp`
5. Rebuild and run `./test_keyframe`

## Testing

The keyframe builder has a standalone test binary that doesn't require OpenCV:

```bash
g++ -o test_keyframe test/test_keyframe.cpp -Isrc -std=c++17 -O2
./test_keyframe
```

This exercises clamping, focus computation, zoom calculation, motion styles, curve generation, and integration through `build_keyframe`.

The IPC protocol has an integration test that drives a headless kbr through the acknowledgment and preload-identity paths, including rejected commands, superseded preloads, and failed image loads:

```bash
python test/test_protocol.py ./kbr demo/images
```

It expects a `huge_noise.png` in the image directory alongside the generated demo images — any large, slow-to-decode image will do — so that preloads take long enough to exercise the race windows.

## Project structure

```
kenburns-renderer/
├── CMakeLists.txt
├── README.md
├── src/
│   ├── main.cpp
│   ├── commands.h
│   ├── keyframe_builder.h
│   ├── renderer.h
│   └── slideshow.h
├── test/
│   ├── test_keyframe.cpp
│   └── test_protocol.py
└── demo/
    ├── demo.jl
    ├── generate_images.py
    └── images/           (generated, not committed)
```

## Process lifecycle

The program calls `_exit(0)` on ESC press, `q` press, quit command, or idle timeout. This bypasses C++ destructors and OpenCV cleanup, which is deliberate — the Cocoa backend on macOS can hang during teardown. The OS reclaims all resources. The controlling process should detect the child's exit via `process_running()` or equivalent.

# License
MIT
