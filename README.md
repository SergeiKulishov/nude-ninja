# FullBlur Filter (OBS plugin)

An OBS source filter for streamers: delays video and audio, runs frames through
an NSFW detector (NudeNet via ONNX Runtime + DirectML), and pixelates/blurs the
frame — either full-frame or only the detected regions — before it reaches the
stream.

Use case: streaming movies or any content where unexpected adult frames must
not be broadcast.

## Disclaimer

FullBlur is a **moderation aid**, not a guarantee. The detector may miss
forbidden content (false negatives) and may blur harmless content (false
positives). **The streamer is solely responsible for the broadcast.** Use the
filter as an extra safety layer, not as the only one.

## NudeNet model (first-run download)

The `320n.onnx` model (~12 MB) is **not bundled** (the NudeNet repository is
GPL-3.0, which is incompatible with GPL-2.0 libobs when bundled). The first time
the filter is added, the model is downloaded automatically from a HuggingFace
mirror into `%APPDATA%\fullblur-filter\models\` (SHA-256 verified). While the
download is in progress, the filter passes frames through unchanged and shows
progress in the **AI stats** field. Without internet, you can place the model
file in that folder manually.

## Installation

### Installer (recommended)

1. Close OBS Studio.
2. Run `fullblur-filter-0.9.0-setup.exe` as Administrator.
3. Start OBS. Right-click a source → **Filters** → **+** → **FullBlur**.

### Manual ZIP install

1. Close OBS Studio.
2. Copy the archive contents into your OBS folder, e.g.
   `C:\Program Files\obs-studio`:
   - `obs-plugins\64bit\*.dll` → `OBS\obs-plugins\64bit\`
   - `data\obs-plugins\fullblur-filter\` → `OBS\data\obs-plugins\fullblur-filter\`
3. Start OBS and add the filter.

Requirements: OBS Studio 32.x, Windows 10/11 x64. A GPU is not required (CPU
inference will be used), but DirectML greatly reduces load.

## Audio (important!)

The filter delays only the audio that passes through the source itself.

- **Option A (recommended):** in the source properties (Game/Window Capture)
  enable **Capture audio**, and mute **Desktop Audio** in the Audio Mixer.
- **Option B:** add an *Application Audio Capture* source and apply FullBlur to
  it as well (this delays only that app's audio by the same amount).

To hear the delayed audio locally: *Edit → Advanced Audio Properties → Audio
Monitoring → Monitor and Output*.

## Settings

| Parameter | Description | Default |
|---|---|---|
| Delay | A/V delay (time for AI analysis) | 2.0 s |
| Blur strength | Pixelation strength | 14 |
| Blur mode | `Full frame` / `Detected areas` | Full frame |
| Area margin | Expand detected regions (motion compensation) | 1.5 |
| AI confidence | Detection threshold (effective minimum 0.25) | 0.2 |
| Hold blur | Keep blurring after the last NSFW detection | 2.0 s |
| Process every Nth | Analyze every Nth frame | 2 |
| AI detail | Tile grid: `Single`, `2x2`, `3x3` — catches small objects. A full-frame pass is always added; tiles overlap by 12% | 2x2 |
| AI model | `320n` — fast (CPU/DirectML); `640m` — accurate, 640×640 input, **GPU only** (~99 MB, downloaded on selection). For 640m use Single/2x2 + Process every 3–4 | 320n |
| Temporal voting | Require NSFW in K of N recent analyses per tile to reduce flicker | 2 of 3 |
| Strong hit bypass | Confident NSFW (score ≥ threshold) blurs instantly, bypassing voting | on, 0.5 |
| Force blur | Force blur for testing | off |
| Classes preset | `Recommended` / `Exposed only` / `All 18` / `Custom` | Recommended |
| Blur classes | 18 NudeNet class checkboxes | 9 classes |
| Blur unverified frames | Fail-safe: blur frames AI didn't check | off |
| Blur now! | Panic button: blur for N seconds immediately | 10 s |
| AI stats | Provider, inference time, counters | — |

Notes:

- The delay buffer stores frames in VRAM: ~0.5 GB per second of delay at 1080p60.
  Reduce Delay if you run out of memory.
- **AI detail and load.** Each tile is a separate inference (~5–15 ms on
  DirectML). At 60 fps: `2x2` with Process every = 2–4 means 60–120 inferences/s,
  `3x3` with 6–10 means 54–90/s. If `dropped` in AI stats grows, AI can't keep
  up: increase Process every or lower detail. `3x3` catches objects as small as
  ~3–4% of the frame.
- `Detected areas` keeps the stream watchable, but on very fast motion the box
  may lag by a few frames — increase Area margin or use `Full frame` for 100%
  safety.
- Apply the filter per source, group, or scene as needed.

## Building from source

Requires: Visual Studio 2022/2026 (C++), CMake 3.28+, internet (first configure
downloads obs-studio 32.1.2, obs-deps, ONNX Runtime 1.22.1, DirectML 1.15.4).

```bat
cmake --preset windows-x64
cmake --build --preset windows-x64
install.bat   :: run as Administrator
```

## Architecture

```
source → filter:
  video_render: capture frame (texrender) → GPU ring-buffer (delay)
              → every Nth frame: tile atlas (320×320 cells)
                → staging → tiling → AI worker (FIFO 32, drop-oldest)
              → output frame older than delay; blur by verdict+bbox / hold / panic
  filter_audio: FIFO with same delay (OBS timestamps → sync)
AI worker (thread): ONNX Runtime (DirectML→CPU fallback), NudeNet 320n,
                    top-8 bbox per tile, OR aggregation across frame tiles
```

Files: `src/fullblur-filter.c` — filter (delay, blur, UI),
`src/ai-worker.cpp` — inference thread, `src/model-download.cpp` — first-run
download of `320n.onnx`.

## Third-party licenses

`THIRD_PARTY_LICENSES/` contains the MIT licenses for ONNX Runtime and DirectML.
The NudeNet model is downloaded separately at runtime.

See [README_RU.md](README_RU.md) for the Russian version.
