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

if defined BEAN_FFMPEG_PATH (
  if exist "%BEAN_FFMPEG_PATH%" (
    set "FFMPEG_SOURCE_EXE=%BEAN_FFMPEG_PATH%"
  )
)

if not defined FFMPEG_SOURCE_EXE (
  if exist "tools\ffmpeg\bin\ffmpeg.exe" (
    set "FFMPEG_SOURCE_EXE=tools\ffmpeg\bin\ffmpeg.exe"
  )
)

if not defined FFMPEG_SOURCE_EXE (
  if exist "C:\Program Files\ffmpeg\bin\ffmpeg.exe" (
    set "FFMPEG_SOURCE_EXE=C:\Program Files\ffmpeg\bin\ffmpeg.exe"
  )
)

if not defined FFMPEG_SOURCE_EXE (
  if exist "C:\Program Files (x86)\ffmpeg\bin\ffmpeg.exe" (
    set "FFMPEG_SOURCE_EXE=C:\Program Files (x86)\ffmpeg\bin\ffmpeg.exe"
  )
)

if not defined FFMPEG_SOURCE_EXE (
  if exist "C:\ProgramData\chocolatey\bin\ffmpeg.exe" (
    set "FFMPEG_SOURCE_EXE=C:\ProgramData\chocolatey\bin\ffmpeg.exe"
  )
)

if not defined FFMPEG_SOURCE_EXE (
  if defined BEAN_OBS_ROOT (
    if exist "%BEAN_OBS_ROOT%\bin\64bit\ffmpeg.exe" (
      set "FFMPEG_SOURCE_EXE=%BEAN_OBS_ROOT%\bin\64bit\ffmpeg.exe"
    )
  )
)

if not defined FFMPEG_SOURCE_EXE (
  if exist "C:\Program Files\obs-studio\bin\64bit\ffmpeg.exe" (
    set "FFMPEG_SOURCE_EXE=C:\Program Files\obs-studio\bin\64bit\ffmpeg.exe"
  )
)

if not defined FFMPEG_SOURCE_EXE (
  for /f "delims=" %%I in ('where ffmpeg.exe 2^>nul') do (
    if not defined FFMPEG_SOURCE_EXE (
      set "FFMPEG_SOURCE_EXE=%%~fI"
    )
  )
)

if not defined FFMPEG_SOURCE_EXE (
  echo [bean] Could not locate ffmpeg.exe to bundle.
  echo [bean] Install ffmpeg or set BEAN_FFMPEG_PATH, then retry.
  exit /b 1
)

for %%I in ("%FFMPEG_SOURCE_EXE%") do set "FFMPEG_SOURCE_DIR=%%~dpI"
if not exist "%FFMPEG_SOURCE_DIR%\ffmpeg.exe" (
  echo [bean] Resolved ffmpeg source is invalid: "%FFMPEG_SOURCE_EXE%"
  exit /b 1
)
exit /b 0
