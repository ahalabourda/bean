@echo off
setlocal
pushd "%~dp0"

if not exist "build\CMakeCache.txt" (
  echo [bean] Configuring CMake...
  cmake -S . -B build
  if errorlevel 1 goto :fail
)

echo [bean] Building bean_tests...
cmake --build build --target bean_tests
if errorlevel 1 goto :fail

echo [bean] Running bean_tests...
build\Debug\bean_tests.exe
if errorlevel 1 goto :fail

echo.
echo [bean] Unit tests passed.
goto :done

:fail
echo.
echo [bean] Unit tests failed.
popd
if not defined BEAN_NO_PAUSE pause
exit /b 1

:done
popd
if not defined BEAN_NO_PAUSE pause
exit /b 0
