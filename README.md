# Ken Burns Slideshow — C++ Renderer

A standalone C++ program that renders a Ken Burns effect slideshow with mipmap-based anti-aliasing, directional motion blur, smooth crossfade transitions, and background image preloading. Designed to be controlled by an external process via file-based IPC.

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

The program opens a fullscreen OpenCV window and enters its render loop. It reads commands from `<command_dir>/command.json` and writes status to `<command_dir>/status.json` and events to `<command_dir>/events.log`. Press ESC in the window to quit immediately.

## Architecture

Four source files, one compilation unit:

| File                 | Purpose                                                |
|----------------------|--------------------------------------------------------|
| `slideshow.h`        | Types, mipmap builder, state machine, preloader thread |
| `keyframe_builder.h` | Focal point, zoom, and motion computation              |
| `renderer.h`         | Pyramid-aware rendering with motion blur               |
| `commands.h`         | File-based command/status/event I/O with kqueue        |
| `main.cpp`           | Main loop wiring everything together                   |

### Render pipeline

Each frame follows this path:

1. State machine `tick()` produces a `RenderParams` struct describing what to draw (which pyramids, interpolation parameter, blend alpha, etc.)
2. Renderer computes a `CropState` via `interpolate_crop` with smoothstep easing
3. Selects the appropriate mipmap level (`log2` of downsample ratio) to avoid aliasing
4. Builds an affine matrix mapping output pixels back to the pyramid level
5. `warpAffine` with `BORDER_CONSTANT` fills black for any out-of-bounds regions
6. If crossfading, renders both images and blends with `addWeighted`
7. If motion blur is enabled, computes a directional kernel from the frame-to-frame velocity and applies `filter2D`

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

Written every frame to `<command_dir>/status.json` (atomic rename). Useful for monitoring and debugging.

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

| Event              | Meaning                                |
|--------------------|----------------------------------------|
| `phase idle`       | Entered idle state                     |
| `phase holding`    | Entered holding state                  |
| `phase transitioning` | Entered transitioning state         |
| `preload_ready`    | Next image pyramid is built and ready  |
| `skipped`          | Skip command was processed             |
| `key <code>`       | A key was pressed in the OpenCV window |

Key code 27 is ESC (triggers immediate `_exit(0)`). Key code 32 is spacebar.

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

The program calls `_exit(0)` on ESC press, quit command, or idle timeout. This bypasses all C++ destructors and Qt/OpenCV cleanup, which is deliberate — Qt's Cocoa backend on macOS can hang during teardown. The OS reclaims all resources. The controlling process should detect the child's death via `process_running()` or equivalent.
