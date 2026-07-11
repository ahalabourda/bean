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

echo [bean] Building and running log simulation...
cmake --build build --target bean_log_replay_dump
if errorlevel 1 goto :fail
build\Debug\bean_log_replay_dump.exe
if errorlevel 1 goto :fail

echo.
echo [bean] All checks completed successfully.
echo [bean] Simulation output: tests\WoWCombatLog-061926_232002.detected-statuses.txt
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
