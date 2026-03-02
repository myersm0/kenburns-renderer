# Ken Burns Slideshow — C++ Renderer

A standalone C++ program that renders a Ken Burns effect slideshow with mipmap-based anti-aliasing, directional motion blur, smooth crossfade transitions, and background image preloading. Designed to be controlled by an external process (typically Julia) via file-based IPC.

## Building

Requires OpenCV 4.x with highgui, imgproc, and imgcodecs.

```bash
g++ -o slideshow main.cpp \
    -I/path/to/opencv/include/opencv4 \
    -L/path/to/opencv/lib \
    -Wl,-rpath,/path/to/opencv/lib \
    -lopencv_core -lopencv_imgproc -lopencv_highgui -lopencv_imgcodecs \
    -std=c++17 -pthread -O2
```

## Usage

```bash
./slideshow <command_dir> [options]
```

**Options:**

| Flag        | Default | Description                                      |
|-------------|---------|--------------------------------------------------|
| `--width`   | 1920    | Output resolution width                          |
| `--height`  | 1080    | Output resolution height                         |
| `--fps`     | 30      | Target frame rate                                |
| `--hold`    | 5.0     | Seconds to hold each image before transitioning  |
| `--fade`    | 3.0     | Crossfade duration in seconds                    |
| `--timeout` | 300.0   | Seconds of inactivity before auto-quit           |

The program opens a fullscreen OpenCV window and enters its render loop. It reads commands from `<command_dir>/command.json` and writes status to `<command_dir>/status.json` and events to `<command_dir>/events.log`.

## Keyboard controls

| Key        | Action                                          |
|------------|-------------------------------------------------|
| Spacebar   | Toggle pause (freezes animation on current frame) |
| `?`        | Toggle debug overlay                            |
| `q` or ESC | Quit immediately                                |

Spacebar, `?`, `q`, and ESC are consumed by the renderer and not forwarded to the controller. All other keypresses are written to the event log for the controller to handle.

## Debug overlay

Press `?` during playback to toggle the debug overlay. Combine with spacebar pause to freeze a frame and inspect the geometry. The overlay draws:

| Element                | Color        | Meaning                                              |
|------------------------|--------------|------------------------------------------------------|
| Thin rectangle         | Gray         | Image boundary — the full extent of the source image in output space. Visible when the crop extends beyond the image (zoom < 1.0 or aspect mismatch). |
| Filled circles         | Red / white  | Focal points — the normalized coordinates passed via the `points` field in the load command. These are the regions of interest the keyframe builder was asked to frame. |
| Rectangle around dots  | Yellow       | Bounding box of focal points (shown when 2+ points are present). This is the region that `fit_points` zoom tries to contain. |
| Cross marker           | Green        | Start position — where the crop center begins at the start of the animation (keyframe `start_x`, `start_y`). |
| Tilted cross marker    | Blue         | End position — where the crop center will be at the end of the animation (keyframe `end_x`, `end_y`). |
| Line between markers   | Gray         | Motion path — the trajectory from start to end position. |
| Crosshair at center    | White        | Screen center — the current crop center is always here by definition. As the animation progresses, the start marker drifts one way and the end marker drifts the other. |

**Text readout** (top-left corner):

- **zoom: 1.23  t: 0.45** — Current interpolated zoom level and animation progress (0.0 = start, 1.0 = end). Zoom follows the camera convention: 1.0 = entire image visible (fit to output aspect ratio), 2.0 = half the linear extent visible (2x magnification). Values below 1.0 mean the crop exceeds the image and black borders appear.
- **kf: (0.50,0.50,1.10) → (0.45,0.48,1.30)** — The raw keyframe: (start_x, start_y, start_zoom) → (end_x, end_y, end_zoom). Coordinates are normalized [0,1] relative to the source image. These are the values computed by the keyframe builder (or passed directly in a raw keyframe command).
- **src: 4000x3000  points: 2** — Source image dimensions and number of focal points.

## Zoom semantics

Zoom uses the camera/film convention:

- **zoom = 1.0** — The entire image is visible within the output frame. If the image aspect ratio differs from the output, black bars (letterboxing or pillarboxing) fill the gap. This is a "fit" behavior.
- **zoom = 2.0** — The crop covers half the linear extent of the image in each direction (1/4 the area). The image appears twice as close.
- **zoom < 1.0** — The crop exceeds the image. The full image is visible with increasing black borders.

The reference frame is the largest output-aspect-ratio rectangle that contains the entire source image. This makes zoom values portable across images of different resolutions — zoom 1.2 means the same visual tightness regardless of whether the source is 800x600 or 8000x6000.

For quality-sensitive applications, note that high zoom on low-resolution images produces upscaling artifacts. The crossover point where source pixels map 1:1 to output pixels varies per image and can be computed as `max(img_w / (out_w / out_aspect), img_h) / out_h` where `out_aspect = out_w / out_h`. Zoom values above this threshold are downsampling (sharp), below it are upscaling (soft).

## Architecture

Five source files, one compilation unit:

| File                 | Purpose                                                |
|----------------------|--------------------------------------------------------|
| `slideshow.h`        | Types, mipmap builder, state machine, preloader thread |
| `keyframe_builder.h` | Focal point, zoom, and motion computation              |
| `renderer.h`         | Pyramid-aware rendering with motion blur and debug overlay |
| `commands.h`         | File-based command/status/event I/O with kqueue        |
| `main.cpp`           | Main loop wiring everything together                   |

### Render pipeline

Each frame follows this path:

1. State machine `tick()` produces a `RenderParams` struct describing what to draw (which pyramids, interpolation parameter, blend alpha, debug points, etc.)
2. Renderer computes a `CropState` via `interpolate_crop` with smoothstep easing
3. Selects the appropriate mipmap level (`log2` of downsample ratio) to avoid aliasing
4. Builds an affine matrix mapping output pixels back to the pyramid level
5. `warpAffine` with `BORDER_CONSTANT` fills black for any out-of-bounds regions
6. If crossfading, renders both images and blends with `addWeighted`
7. If motion blur is enabled, computes a directional kernel from the frame-to-frame velocity and applies `filter2D`
8. If debug mode is active, draws the overlay on top of the final frame

### Mipmap pyramid

Each image is decomposed into a Gaussian pyramid via `pyrDown`. Per frame, the renderer selects the level whose resolution is closest to 1:1 with the output crop, eliminating aliasing artifacts on fine detail (foliage, hair, fabric textures). Memory overhead is 1.33x the source image. The preloader thread builds the next image's pyramid in the background during the hold phase.

### State machine

`SlideshowState` manages three phases:

- **Idle** — waiting for the first image
- **Holding** — displaying an image with Ken Burns animation
- **Transitioning** — crossfading between current and next image

Transitions are explicitly triggered via the `transition` command. The state machine never auto-advances — the controlling process decides timing.

## IPC Protocol

### Commands (controller → slideshow)

Write JSON to `<command_dir>/command.json` (via atomic rename from `.tmp`). The slideshow deletes the file after reading.

**Load with style (C++ computes keyframe):**

```json
{
    "command": "load",
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
    "path": "/absolute/path/to/image.jpg",
    "start_x": 0.5, "start_y": 0.5, "start_zoom": 1.0,
    "end_x": 0.4, "end_y": 0.4, "end_zoom": 1.5
}
```

**Other commands:**

```json
{"command": "transition"}
{"command": "skip"}
{"command": "quit"}
{"command": "config", "key": "blur", "value": 0.3}
```

Config keys: `blur` (motion blur strength), `hold` (hold duration in seconds), `fade` (fade duration in seconds).

### Status (slideshow → controller)

Written every frame to `<command_dir>/status.json` (atomic rename). Useful for monitoring; the controller does not depend on this for control flow.

```json
{
    "phase": "holding",
    "image": "/path/to/current.jpg",
    "last_key": -1,
    "preload_ready": true
}
```

### Events (slideshow → controller)

Appended to `<command_dir>/events.log`. The controller tracks its read position and consumes new lines. Events are edge-triggered — they fire once per state change.

| Event                  | Meaning                                |
|------------------------|----------------------------------------|
| `phase idle`           | Entered idle state                     |
| `phase holding`        | Entered holding state                  |
| `phase transitioning`  | Entered transitioning state            |
| `preload_ready`        | Next image pyramid is built and ready  |
| `skipped`              | Skip command was processed             |
| `paused`               | Playback paused via spacebar           |
| `resumed`              | Playback resumed via spacebar          |
| `key <code>`           | A non-consumed key was pressed         |

Note: spacebar (pause/resume), `?` (debug toggle), ESC, and `q` (quit) are consumed by the renderer. Only other keypresses generate `key` events for the controller to handle.

## Keyframe Builder

When a `load` command includes `focus`/`zoom`/`motion` fields, C++ computes the keyframe after loading the image (so it knows the image dimensions). All coordinates are normalized to [0, 1] relative to image dimensions.

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

### Motion styles

| Value      | Behavior                                                |
|------------|---------------------------------------------------------|
| `static`   | No movement — still frame                              |
| `zoom_in`  | Start at 85% zoom, end at 115%                        |
| `zoom_out` | Start at 115% zoom, end at 85%                        |
| `drift`    | Random gentle pan and zoom shift (magnitude controlled by `drift_magnitude`) |
| `pan_to`   | Pan from first point to second point in `points`       |

All keyframes are automatically clamped to avoid unnecessary black borders. When the crop fits inside the image, the center is pushed inward so edges don't exceed bounds. When the crop is larger than the image (low zoom), it centers and lets `BORDER_CONSTANT` fill black.

### Extending with new methods

To add a new focus method, zoom method, or motion style:

1. Add the enum value in `keyframe_builder.h` (e.g. `FocusMethod::FaceDetect`)
2. Add the corresponding case in `compute_focus`, `compute_zoom`, or `apply_motion`
3. Add the string mapping in `commands.h` parser (e.g. `if (focus == "face_detect") ...`)
4. Add a test case in `test_keyframe.cpp`
5. Rebuild and run `./test_keyframe`

Example — adding a weighted centroid focus method:

```cpp
// in keyframe_builder.h
enum class FocusMethod { Center, Random, Specific, Union, WeightedCentroid };

// in compute_focus
case FocusMethod::WeightedCentroid: {
    if (points.empty()) return {0.5, 0.5};
    double sum_x = 0, sum_y = 0;
    for (auto& p : points) {
        sum_x += p.x;
        sum_y += p.y;
    }
    return {sum_x / points.size(), sum_y / points.size()};
}

// in commands.h parser
else if (focus == "weighted_centroid")
    cmd.style.focus = FocusMethod::WeightedCentroid;
```

## Testing

The keyframe builder has a standalone test binary that doesn't require OpenCV's highgui:

```bash
g++ -o test_keyframe test_keyframe.cpp -std=c++17 -O2
./test_keyframe
```

This exercises clamping, focus computation, zoom calculation, motion styles, and integration through `build_keyframe`. Add tests here before adding new methods.

## Process lifecycle

The program calls `_exit(0)` on ESC press, `q` press, quit command, or idle timeout. This bypasses all C++ destructors and Qt/OpenCV cleanup, which is deliberate — Qt's Cocoa backend on macOS can hang during teardown. The OS reclaims all resources. The controlling process should detect the child's death via `process_running()` or equivalent.
