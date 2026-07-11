# Contributing to Bean

Thanks for your interest in Bean.

## Development setup

Bean currently targets Windows 10/11 and requires:

- Visual Studio 2022 with the C++ workload
- CMake 3.24 or newer

Configure, build, and test with:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Please do not commit build directories, generated files, release packages,
credentials, or raw World of Warcraft combat logs. The repository's ignore
rules cover these files.

## Pull requests

- Explain the user-visible behavior or problem being addressed.
- Include or update tests when behavior changes.
- Keep changes focused and buildable on Windows.
- Confirm that `ctest --test-dir build -C Debug --output-on-failure` passes.

By submitting a contribution, you agree that it may be distributed under the
GNU General Public License version 3 or any later version.
