@echo off
setlocal
pushd "%~dp0"

set "SDK_ROOT=tools\velopack-sdk"
set "SDK_ZIP=tools\velopack_libc_1.2.0.zip"
set "SDK_URL=https://github.com/velopack/velopack/releases/download/1.2.0/velopack_libc_1.2.0.zip"

if not exist "tools" mkdir "tools"
if exist "%SDK_ROOT%" (
  echo [bean] Removing existing "%SDK_ROOT%"...
  rmdir /S /Q "%SDK_ROOT%"
)

echo [bean] Downloading Velopack C/C++ SDK 1.2.0...
powershell -NoProfile -ExecutionPolicy Bypass -Command "Invoke-WebRequest -Uri '%SDK_URL%' -OutFile '%SDK_ZIP%'"
if errorlevel 1 goto :fail

echo [bean] Extracting SDK...
powershell -NoProfile -ExecutionPolicy Bypass -Command "Expand-Archive -Path '%SDK_ZIP%' -DestinationPath '%SDK_ROOT%' -Force"
if errorlevel 1 goto :fail

del /Q "%SDK_ZIP%" >nul 2>&1
if not exist "%SDK_ROOT%\include\Velopack.h" (
  echo [bean] Extraction failed. Velopack.h not found.
  goto :fail
)

echo [bean] Velopack SDK ready at "%SDK_ROOT%".
goto :done

:fail
echo.
echo [bean] Failed to download/setup Velopack SDK.
popd
exit /b 1

:done
popd
exit /b 0
