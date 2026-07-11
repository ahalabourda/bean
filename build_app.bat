@echo off
setlocal
pushd "%~dp0"

if not exist "build\CMakeCache.txt" (
  echo [bean] Configuring CMake...
  cmake -S . -B build
  if errorlevel 1 goto :fail
)

echo [bean] Building bean_app...
cmake --build build --target bean_app
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
