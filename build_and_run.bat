@echo off
setlocal
pushd "%~dp0"

echo [bean] Configuring CMake (libobs enabled)...
cmake -S . -B build -DBEAN_ENABLE_LIBOBS=ON
if errorlevel 1 goto :fail

echo [bean] Closing existing bean (if running)...
taskkill /IM bean.exe /F >nul 2>&1

echo [bean] Building bean_app...
cmake --build build --config Debug --target bean_app
if errorlevel 1 goto :fail

echo [bean] Launching bean...
start "" "build\Debug\bean.exe"
if errorlevel 1 goto :fail

echo.
echo [bean] Build and run complete.
goto :done

:fail
echo.
echo [bean] Build and run failed.
popd
exit /b 1

:done
popd
exit /b 0
