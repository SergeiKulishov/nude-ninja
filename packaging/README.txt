FullBlur Filter 0.9.0 beta for OBS Studio 32.x (Windows x64)
=============================================================

FullBlur is an OBS source filter that delays video and audio, runs
NSFW detection (NudeNet ONNX), and pixelates/blurs the frame when
needed. Use case: streaming movies/content where unexpected adult
frames must not reach the stream.

Requirements
------------
- OBS Studio 32.x 64-bit
- Windows 10/11 x64
- A DirectML-capable GPU is strongly recommended (ONNX Runtime uses
  DirectML by default; CPU inference is very slow)

Installation (manual, from ZIP)
-------------------------------
1. Close OBS Studio.
2. Copy the contents of this archive into your OBS installation folder,
   e.g. C:\Program Files\obs-studio
   - obs-plugins\64bit\*.dll go to OBS\obs-plugins\64bit\
   - data\obs-plugins\fullblur-filter\ go to OBS\data\obs-plugins\fullblur-filter\
3. Start OBS, add the "FullBlur Filter" to a source.
4. The first time the filter is active it will download the detection
   model (~12 MB for 320n, ~100 MB for 640m) to:
      %APPDATA%\fullblur-filter\models\
   An "AI stats" line shows the download progress.

Uninstallation
--------------
- Delete fullblur-filter.dll, onnxruntime.dll,
  onnxruntime_providers_shared.dll and DirectML.dll from
  OBS\obs-plugins\64bit\
- Delete OBS\data\obs-plugins\fullblur-filter\

Quick settings for first test
-----------------------------
- Delay: 300-500 ms
- AI detail: Single or 2x2
- Process every: 3 or 4
- AI model: 320n (faster) / 640m (more accurate, needs GPU)

Disclaimer
----------
FullBlur is a moderation aid, not a 100% guarantee. The streamer is
solely responsible for the broadcasted content. Always combine with
human supervision and platform-appropriate delays.

Third-party licenses
--------------------
THIRD_PARTY_LICENSES\ contains licenses for ONNX Runtime and DirectML
(MIT). The NudeNet model is downloaded separately at runtime.
