@echo off
setlocal
pushd "%~dp0.."

echo [bean] Configuring CMake (libobs enabled)...
cmake -S . -B build -DBEAN_ENABLE_LIBOBS=ON
if errorlevel 1 goto :fail

echo [bean] Building app...
cmake --build build --config Debug --target bean_app
if errorlevel 1 goto :fail

echo [bean] Building test targets...
cmake --build build --config Debug --target bean_tests --target bean_core_public_api_tests --target bean_core_logic_tests --target bean_app_helpers_tests
if errorlevel 1 goto :fail

echo [bean] Running ctest...
ctest --test-dir build -C Debug --output-on-failure
if errorlevel 1 goto :fail

echo.
echo [bean] All checks completed successfully.
goto :done

:fail
echo.
echo [bean] One or more steps failed.
popd
if not defined BEAN_NO_PAUSE pause
exit /b 1

:done
popd
if not defined BEAN_NO_PAUSE pause
exit /b 0
