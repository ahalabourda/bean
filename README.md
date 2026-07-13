# Battle Encounter Archival Nexus (Bean)

Bean automatically records your World of Warcraft Mythic+ runs so you can
review them later, make clips, or share them.

## For users

### Download and install

Download the latest release from the [Releases](https://github.com/ahalabourda/bean/releases)
page. Most users should use the Windows installer. A portable download is also
available if you do not want to install the app.

Bean is made for:

- Windows 10 or Windows 11.
- World of Warcraft Retail.
- OBS Studio, installed separately. Bean uses OBS to record your game.

FFmpeg is included with release downloads and is used for previewing and
creating clips.

### How it works

1. Open Bean and go to **Config**.
2. Check that Bean found your WoW folder and choose the recording settings you
   want. Bean will tell you if something is missing.
3. Ensure **Advanced Combat Logging** is enabled in WoW.
4. Ensure Bean has started monitoring your combat log (this should be automatic).
5. Run your Mythic+ key. Bean starts recording when the run begins and stops
   after it ends, including a short period afterward.

You can also manually start and stop a recording yourself at any time. Bean does 
not record until monitoring is turned on or you start a recording manually.

### What you can do

- Record game video and choose what audio to include, such as WoW, Discord,
  all desktop audio, and optionally your microphone.
- Hide chat in recordings with a blank or custom image.
- Browse recordings and see information about each Mythic+ run.
- Preview recordings and trim them into clips.
- Link a YouTube account and upload recordings directly from Bean.

### Important things to know

- OBS Studio must be installed and available when you record. Bean does not
  include OBS.
- Bean watches WoW's combat log to recognize when a Mythic+ run starts and
  ends. If automatic recording does not work, check that Advanced Combat
  Logging is enabled and that Bean is using the correct WoW log folder.
- Recordings are saved on your computer. Make sure the selected drive has
  enough free space.
- YouTube uploads can be private, unlisted, or public. Files larger than 4 GB
  cannot currently be uploaded from Bean.

## For developers

### Requirements

- Windows 10 or 11.
- Visual Studio 2022 with the C++ workload.
- CMake 3.24 or newer.
- OBS Studio installed locally for recording.
- FFmpeg available for the Clips tab. Release builds can download and bundle
  it automatically; development builds need to be able to find it.

### Build, test, and run

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
.\build\Debug\bean.exe
```

To create a release-style build for testing:

```powershell
.\build_release_app.bat
```

The output is staged in `dist\alpha-release`. The script bundles FFmpeg with
the app. It can use a specific executable with `BEAN_FFMPEG_PATH`, change the
download source with `BEAN_FFMPEG_URL`, or disable downloading with
`BEAN_FFMPEG_AUTO_DOWNLOAD=0`.

### OBS configuration

Bean loads OBS from the usual installation locations. To use a different
installation, set `BEAN_OBS_ROOT` to a folder containing
`bin\64bit\obs.dll`. If OBS cannot be found or started, Bean cannot record.

## License

Bean is distributed under the GNU General Public License version 3 or later.
See [LICENSE](LICENSE).
