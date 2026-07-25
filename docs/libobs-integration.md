# libobs Integration Notes

This project uses `LibObsRecorderEngine` by default and dynamically loads OBS runtime APIs from an installed OBS distribution.

## Planned integration steps

1. Locate OBS root (`BEAN_OBS_ROOT` or `C:\Program Files\obs-studio`).
2. Load `obs.dll` from `bin/64bit`.
3. Register/load plugin modules from:
   - `obs-plugins/64bit`
   - `data/obs-plugins/%module%`
4. Initialize core/video/audio:
   - `obs_startup`
   - `obs_reset_video`
   - `obs_reset_audio`
5. Build recording graph:
   - `game_capture` source
   - `wasapi_output_capture` source
   - scene + output channel assignment
   - `ffmpeg_muxer` output
   - `obs_x264` + `ffmpeg_aac` encoders

## CMake switch

- `BEAN_ENABLE_LIBOBS` (default **ON**) selects the recorder engine wired into the app:
  - **ON** → `LibObsRecorderEngine`
  - **OFF** → `MockRecorderEngine` (useful for CI hosts without OBS)
- Both engines are always compiled into `bean_core`. Unit tests construct `MockRecorderEngine` directly.
- Runtime dependency for the libobs path is an installed OBS distribution with compatible binaries/plugins.

## Safety behavior

If OBS runtime cannot be located/initialized, `LibObsRecorderEngine` returns clear status errors and recording does not start.
