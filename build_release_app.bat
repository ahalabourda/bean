@echo off
setlocal
pushd "%~dp0"

set "BUILD_DIR=%BEAN_BUILD_DIR%"
if not defined BUILD_DIR set "BUILD_DIR=build-release"
set "DIST_DIR=%BEAN_DIST_DIR%"
if not defined DIST_DIR set "DIST_DIR=dist\alpha-release"
set "BEAN_CMAKE_ARGS=%BEAN_CMAKE_ARGS%"
set "RELEASE_EXE=%BUILD_DIR%\Release\bean.exe"
set "SINGLE_CONFIG_EXE=%BUILD_DIR%\bean.exe"

if not exist "%BUILD_DIR%\CMakeCache.txt" (
  echo [bean] Configuring CMake for Release...
  cmake -S . -B "%BUILD_DIR%" %BEAN_CMAKE_ARGS%
  if errorlevel 1 goto :fail
)

echo [bean] Building bean_app (Release)...
cmake --build "%BUILD_DIR%" --config Release --target bean_app
if errorlevel 1 goto :fail

if exist "%RELEASE_EXE%" (
  set "SOURCE_EXE=%RELEASE_EXE%"
) else if exist "%SINGLE_CONFIG_EXE%" (
  set "SOURCE_EXE=%SINGLE_CONFIG_EXE%"
) else (
  echo [bean] Could not find Release executable after build.
  goto :fail
)

echo [bean] Closing existing bean (if running)...
taskkill /IM bean.exe /F >nul 2>&1

echo [bean] Staging alpha release to "%DIST_DIR%"...
if exist "%DIST_DIR%" (
  rmdir /S /Q "%DIST_DIR%"
  if exist "%DIST_DIR%" (
    echo [bean] Could not clean existing "%DIST_DIR%" ^(files may be locked^).
    goto :fail
  )
)
mkdir "%DIST_DIR%"
if errorlevel 1 goto :fail

if not exist "LICENSE" (
  echo [bean] Required LICENSE file is missing.
  goto :fail
)
if not exist "THIRD-PARTY-NOTICES.md" (
  echo [bean] Required THIRD-PARTY-NOTICES.md file is missing.
  goto :fail
)
copy /Y "LICENSE" "%DIST_DIR%\LICENSE" >nul
if errorlevel 1 goto :fail
copy /Y "THIRD-PARTY-NOTICES.md" "%DIST_DIR%\THIRD-PARTY-NOTICES.md" >nul
if errorlevel 1 goto :fail

copy /Y "%SOURCE_EXE%" "%DIST_DIR%\bean.exe" >nul
if errorlevel 1 goto :fail

for %%F in ("%BUILD_DIR%\Release\velopack_libc.dll" "%BUILD_DIR%\Release\Velopack.dll" "%BUILD_DIR%\Release\velopack_libc_win_x64_msvc.dll" "%BUILD_DIR%\velopack_libc.dll" "%BUILD_DIR%\Velopack.dll" "%BUILD_DIR%\velopack_libc_win_x64_msvc.dll") do (
  if exist "%%~fF" (
    echo [bean] Bundling Velopack runtime from "%%~fF"...
    copy /Y "%%~fF" "%DIST_DIR%\velopack_libc.dll" >nul
    if errorlevel 1 goto :fail
  )
)

call :resolve_ffmpeg_source
if errorlevel 1 goto :fail

echo [bean] Bundling FFmpeg from "%FFMPEG_SOURCE_DIR%"...
copy /Y "%FFMPEG_SOURCE_DIR%\ffmpeg.exe" "%DIST_DIR%\ffmpeg.exe" >nul
if errorlevel 1 goto :fail
if not exist "%DIST_DIR%\ffmpeg.exe" (
  echo [bean] Bundled FFmpeg executable is missing.
  goto :fail
)
echo [bean] Validating bundled FFmpeg...
"%DIST_DIR%\ffmpeg.exe" -hide_banner -version >nul 2>&1
if errorlevel 1 (
  echo [bean] Bundled FFmpeg failed its startup validation.
  goto :fail
)
if exist "%FFMPEG_SOURCE_DIR%\ffprobe.exe" (
  copy /Y "%FFMPEG_SOURCE_DIR%\ffprobe.exe" "%DIST_DIR%\ffprobe.exe" >nul
  if errorlevel 1 goto :fail
)
for %%F in ("%FFMPEG_SOURCE_DIR%\*.dll") do (
  copy /Y "%%~fF" "%DIST_DIR%\" >nul
  if errorlevel 1 goto :fail
)

if exist "%BUILD_DIR%\Release\assets" (
  xcopy /E /I /Y "%BUILD_DIR%\Release\assets" "%DIST_DIR%\assets" >nul
  if errorlevel 1 goto :fail
) else if exist "%BUILD_DIR%\assets" (
  xcopy /E /I /Y "%BUILD_DIR%\assets" "%DIST_DIR%\assets" >nul
  if errorlevel 1 goto :fail
)

echo.
echo [bean] Release build finished successfully.
echo [bean] Distributable output: "%DIST_DIR%"
goto :done

:fail
echo.
echo [bean] Release build failed.
popd
exit /b 1

:done
popd
exit /b 0

:resolve_ffmpeg_source
set "FFMPEG_SOURCE_EXE="
set "FFMPEG_SOURCE_DIR="

if exist "tools\ffmpeg\bin\ffmpeg.exe" (
  set "FFMPEG_SOURCE_EXE=tools\ffmpeg\bin\ffmpeg.exe"
)

if not defined FFMPEG_SOURCE_EXE (
  call :download_ffmpeg
  if errorlevel 1 exit /b 1
  if exist "tools\ffmpeg\bin\ffmpeg.exe" (
    set "FFMPEG_SOURCE_EXE=tools\ffmpeg\bin\ffmpeg.exe"
  )
)

if not defined FFMPEG_SOURCE_EXE (
  echo [bean] Could not locate or acquire ffmpeg.exe to bundle.
  echo [bean] Retry with network access so the bundled copy can be downloaded.
  exit /b 1
)

for %%I in ("%FFMPEG_SOURCE_EXE%") do set "FFMPEG_SOURCE_DIR=%%~dpI"
if not exist "%FFMPEG_SOURCE_DIR%\ffmpeg.exe" (
  echo [bean] Resolved ffmpeg source is invalid: "%FFMPEG_SOURCE_EXE%"
  exit /b 1
)
exit /b 0

:download_ffmpeg
set "FFMPEG_URL=%BEAN_FFMPEG_URL%"
if not defined FFMPEG_URL set "FFMPEG_URL=https://www.gyan.dev/ffmpeg/builds/packages/ffmpeg-8.1.2-essentials_build.zip"
set "FFMPEG_SHA256_URL=%BEAN_FFMPEG_SHA256_URL%"
set "FFMPEG_SHA256_EXPECTED=%BEAN_FFMPEG_SHA256%"
if not defined FFMPEG_SHA256_EXPECTED if not defined FFMPEG_SHA256_URL if defined BEAN_FFMPEG_URL (
  echo [bean] A custom FFmpeg URL requires BEAN_FFMPEG_SHA256 or BEAN_FFMPEG_SHA256_URL.
  exit /b 1
)
if not defined FFMPEG_SHA256_EXPECTED if not defined FFMPEG_SHA256_URL set "FFMPEG_SHA256_EXPECTED=db580001caa24ac104c8cb856cd113a87b0a443f7bdf47d8c12b1d740584a2ec"
set "FFMPEG_ZIP=tools\ffmpeg-release-essentials.zip"
set "FFMPEG_SHA256_FILE=tools\ffmpeg-release-essentials.zip.sha256"
set "FFMPEG_TEMP=tools\ffmpeg-download"

if exist "tools\ffmpeg\bin\ffmpeg.exe" (
  echo [bean] Using cached FFmpeg in "tools\ffmpeg".
  exit /b 0
)

if not exist "tools" mkdir "tools"
if exist "%FFMPEG_TEMP%" rmdir /S /Q "%FFMPEG_TEMP%"
echo [bean] FFmpeg was not found; downloading release essentials build...
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ProgressPreference='SilentlyContinue'; Invoke-WebRequest -UseBasicParsing -TimeoutSec 60 -Uri '%FFMPEG_URL%' -OutFile '%FFMPEG_ZIP%'"
if errorlevel 1 (
  echo [bean] Failed to download FFmpeg from "%FFMPEG_URL%".
  exit /b 1
)

if not defined FFMPEG_SHA256_EXPECTED (
  echo [bean] Downloading FFmpeg checksum...
  powershell -NoProfile -ExecutionPolicy Bypass -Command "$ProgressPreference='SilentlyContinue'; Invoke-WebRequest -UseBasicParsing -TimeoutSec 60 -Uri '%FFMPEG_SHA256_URL%' -OutFile '%FFMPEG_SHA256_FILE%'"
  if errorlevel 1 (
    echo [bean] Failed to download FFmpeg checksum from "%FFMPEG_SHA256_URL%".
    exit /b 1
  )
  for /f "usebackq delims=" %%H in ("%FFMPEG_SHA256_FILE%") do if not defined FFMPEG_SHA256_EXPECTED set "FFMPEG_SHA256_EXPECTED=%%H"
)

for /f "tokens=1" %%H in ("%FFMPEG_SHA256_EXPECTED%") do set "FFMPEG_SHA256_EXPECTED=%%H"
echo [bean] Verifying FFmpeg checksum...
powershell -NoProfile -ExecutionPolicy Bypass -Command "$expected='%FFMPEG_SHA256_EXPECTED%'.Trim().ToLowerInvariant(); $actual=(Get-FileHash -LiteralPath '%FFMPEG_ZIP%' -Algorithm SHA256).Hash.ToLowerInvariant(); if ($actual -ne $expected) { Write-Error ('Checksum mismatch for %FFMPEG_ZIP%: expected ' + $expected + ', got ' + $actual); exit 1 }"
if errorlevel 1 exit /b 1

echo [bean] Extracting FFmpeg...
powershell -NoProfile -ExecutionPolicy Bypass -Command "Expand-Archive -Path '%FFMPEG_ZIP%' -DestinationPath '%FFMPEG_TEMP%' -Force"
if errorlevel 1 (
  echo [bean] Failed to extract FFmpeg archive.
  exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -Command "$exe=Get-ChildItem -Path '%FFMPEG_TEMP%' -Filter 'ffmpeg.exe' -Recurse | Select-Object -First 1; if (-not $exe) { exit 1 }; New-Item -ItemType Directory -Force -Path 'tools\ffmpeg\bin' | Out-Null; Copy-Item -Path ($exe.Directory.FullName + '\*') -Destination 'tools\ffmpeg\bin' -Force"
if errorlevel 1 (
  echo [bean] FFmpeg archive did not contain a usable bin directory.
  exit /b 1
)

rmdir /S /Q "%FFMPEG_TEMP%" >nul 2>&1
del /Q "%FFMPEG_ZIP%" >nul 2>&1
del /Q "%FFMPEG_SHA256_FILE%" >nul 2>&1
if not exist "tools\ffmpeg\bin\ffmpeg.exe" (
  echo [bean] FFmpeg acquisition completed without ffmpeg.exe.
  exit /b 1
)
echo [bean] FFmpeg cached in "tools\ffmpeg".
exit /b 0
