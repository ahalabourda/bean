# Battle Encounter Archival Nexus (Bean)

`bean` is a native Windows C++ application that records World of Warcraft Mythic+ runs automatically from combat-log events.

## Current capabilities

Bean provides a native Win32 desktop UI for:

- Output and WoW combat-log folders.
- Hardware/software video encoder selection and quality presets.
- Video width, height, FPS, and MP4/MKV container selection.
- WoW-only, WoW+Discord, or all-desktop audio capture.
- Optional microphone capture with device selection and noise suppression.
- Chat privacy blocking with a blank or custom image, configurable size, and anchor corner.
- Start/stop monitoring and manual start/stop recording.
- Automatic Mythic+ recording start/stop, including a configurable post-run tail.
- A recordings browser with run metadata, participant/spec information, and one-click YouTube upload.
- A clips tab for previewing, selecting, trimming, and exporting portions of recordings.
- Status information for WoW, OBS, FFmpeg, combat logging, and recording state.
- Optional Velopack update checks from the About tab.

The combat-log watcher follows the newest file matching
`WoWCombatLog-######_######.txt`. When monitoring first starts, it tails the
current file rather than replaying its existing history. It handles log
rotation and file changes while monitoring.

## Build

Requirements:

- Windows 10/11.
- Visual Studio 2022 with the C++ workload.
- CMake 3.24 or newer.
- A compatible OBS Studio installation for recording. Bean dynamically loads
  OBS at runtime and does not bundle the OBS installation.
- FFmpeg for clip preview/export. Release builds bundle FFmpeg automatically,
  but development builds must be able to locate it when using the Clips tab.

Configure, build, and test:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Run the app:

```powershell
.\build\Debug\bean.exe
```

The app automatically searches common WoW `_retail_\Logs` locations. If it
cannot find the correct folder, select the WoW log folder in Config. WoW
advanced combat logging must be enabled for combat-log events to be produced.

Create an alpha release build for testers:

```powershell
.\build_release_app.bat
```

The distributable package is staged in `dist\alpha-release`. The script
bundles `ffmpeg.exe`, optional `ffprobe.exe`, and FFmpeg DLLs so clip preview
and trimming work without a separate FFmpeg installation. It fails with setup
instructions if FFmpeg cannot be located.

## OBS runtime

`LibObsRecorderEngine` is the active recorder engine. It dynamically loads
`obs.dll` and the required OBS plugins, including game capture, WASAPI,
FFmpeg output, image sources, and encoders.

Bean searches available drives for:

- `<drive>:\Program Files\obs-studio`
- `<drive>:\Program Files (x86)\obs-studio`

Set `BEAN_OBS_ROOT` to override this search. The override must contain
`bin\64bit\obs.dll`. If OBS cannot be found or initialized, recording does not
start and the UI reports the error. The mock recorder remains available for
test support, but it is not used by the application.

## Velopack installer and autoupdates

Velopack support is optional and is enabled only for specially configured
builds. It adds update checks and update application to the About tab.

1. Download `velopack_libc_{version}.zip` from Velopack releases and extract
   it locally, or run `download_velopack_sdk.bat`.
2. Configure CMake with Velopack enabled:

```powershell
cmake -S . -B build-release `
  -DBEAN_ENABLE_VELOPACK=ON `
  -DBEAN_VELOPACK_SDK_DIR="C:/path/to/velopack_libc" `
  -DBEAN_VELOPACK_UPDATE_URL="https://github.com/ahalabourda/bean/releases"
```

3. Build and package the release payload:

```powershell
.\build_release_app.bat
dotnet tool install -g vpk
.\package_velopack_release.bat <semver-version>
```

`build_and_package_velopack.bat <semver-version>` performs the configured
release build and packaging in one command.

Notes:

- Override the update feed at runtime with `BEAN_UPDATE_URL`.
- GitHub release URLs are recognized automatically; the `/releases` suffix is
  optional.
- `BEAN_VELOPACK_PACK_ID` defaults to `gg.andrew.bean`.
- `BEAN_VELOPACK_VERSION` can supply the package version when no CLI argument
  is provided.
- Velopack packaging outputs to `Releases\`.
- `.github/workflows/release.yml` publishes packages when a `v*` tag is pushed.

## YouTube uploads

Bean can link a YouTube account and upload a selected local recording from the
Recordings tab. The OAuth authorization service is hosted separately from this
repository; there is no `server/youtube-auth` source directory in this
checkout. The desktop application is preconfigured to use the deployed HTTPS
service.

The hosted authorization service must:

1. Use a Google Cloud project with YouTube Data API v3 enabled.
2. Use a Google OAuth web application client.
3. Run behind HTTPS with its callback URL registered.
4. Exchange the authorization code and return the short-lived authorization
   result expected by Bean.

For users, the flow is:

1. Click `Link YouTube` and sign into the desired Google/YouTube account.
2. Select a recording, enter a title, and choose `private`, `unlisted`, or
   `public`.
3. Click `Upload to YouTube`.

Bean never starts a listening socket and does not contain the OAuth client
secret. It stores the returned refresh token using Windows DPAPI and refreshes
YouTube access tokens directly. The uploader currently rejects files larger
than 4 GB.

Official references:

- [YouTube Data API videos.insert](https://developers.google.com/youtube/v3/docs/videos/insert)
- [OAuth 2.0 web server applications](https://developers.google.com/identity/protocols/oauth2/web-server)
- [Google Cloud credentials page](https://console.cloud.google.com/apis/credentials)
- [Enable YouTube Data API v3](https://console.cloud.google.com/apis/library/youtube.googleapis.com)

Notes:

- The authorization flow uses the `youtube.upload` scope.
- End users do not need API keys or their own OAuth client.
- Unverified API projects created after July 28, 2020 are restricted to
  private uploads until the project completes Google's verification process.

## Project layout

- `src/app` - native Windows UI, status panels, clips, and update integration.
- `src/core` - orchestration, settings, run metadata, and persistence.
- `src/integrations` - YouTube authorization and upload integration.
- `src/log` - combat-log watcher and Mythic+ detector.
- `src/obs` - recorder interface, mock engine, and libobs engine.
- `assets/icons` - application icons.
- `assets/spec-icons` - participant specialization icons.
- `tests` - unit tests.
- `docs` - integration notes.
- `.github/workflows` - continuous integration and GitHub release automation.

## License

Bean is distributed under the GNU General Public License version 3 or later.
See [LICENSE](LICENSE).

## Combat logs

Raw `WoWCombatLog-<date>_<time>.txt` files are ignored because they can be
very large and may contain personal player data. Detector behavior is covered
by the focused unit tests in `tests/`.
