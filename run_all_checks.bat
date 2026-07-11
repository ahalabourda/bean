@echo off
setlocal
pushd "%~dp0"

if not exist "build\CMakeCache.txt" (
  echo [bean] Configuring CMake...
  cmake -S . -B build
  if errorlevel 1 goto :fail
)

echo [bean] Building app...
cmake --build build --target bean_app
if errorlevel 1 goto :fail

echo [bean] Building and running unit tests...
cmake --build build --target bean_tests
if errorlevel 1 goto :fail
build\Debug\bean_tests.exe
if errorlevel 1 goto :fail

echo [bean] Building and running core API tests...
cmake --build build --target bean_core_public_api_tests
if errorlevel 1 goto :fail
build\Debug\bean_core_public_api_tests.exe
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
