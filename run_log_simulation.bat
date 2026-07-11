@echo off
setlocal
pushd "%~dp0"

if not exist "build\CMakeCache.txt" (
  echo [bean] Configuring CMake...
  cmake -S . -B build
  if errorlevel 1 goto :fail
)

echo [bean] Building bean_log_replay_dump...
cmake --build build --target bean_log_replay_dump
if errorlevel 1 goto :fail

echo [bean] Running combat log replay simulation...
build\Debug\bean_log_replay_dump.exe
if errorlevel 1 goto :fail

echo.
echo [bean] Simulation complete.
echo [bean] Output file: tests\WoWCombatLog-061926_232002.detected-statuses.txt
goto :done

:fail
echo.
echo [bean] Simulation failed.
popd
if not defined BEAN_NO_PAUSE pause
exit /b 1

:done
popd
if not defined BEAN_NO_PAUSE pause
exit /b 0
