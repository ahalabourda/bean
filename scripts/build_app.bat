@echo off
setlocal
pushd "%~dp0.."

echo [bean] Configuring CMake (libobs enabled)...
cmake -S . -B build -DBEAN_ENABLE_LIBOBS=ON
if errorlevel 1 goto :fail

echo [bean] Building bean_app...
cmake --build build --config Debug --target bean_app
if errorlevel 1 goto :fail

echo.
echo [bean] Build finished successfully.
goto :done

:fail
echo.
echo [bean] Build failed.
popd
exit /b 1

:done
popd
exit /b 0
