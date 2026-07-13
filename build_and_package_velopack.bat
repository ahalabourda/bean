@echo off
setlocal
pushd "%~dp0"

set "PACK_VERSION=%~1"
if not defined PACK_VERSION set "PACK_VERSION=%BEAN_VELOPACK_VERSION%"
if not defined PACK_VERSION (
  echo [bean] Usage: build_and_package_velopack.bat ^<semver-version^>
  echo [bean] Or set BEAN_VELOPACK_VERSION in your environment.
  goto :fail
)

set "SDK_DIR=%BEAN_VELOPACK_SDK_DIR%"
if not defined SDK_DIR set "SDK_DIR=tools\velopack-sdk"
if not exist "%SDK_DIR%\include\Velopack.h" (
  echo [bean] Velopack SDK not found at "%SDK_DIR%".
  if /I not "%SDK_DIR%"=="tools\velopack-sdk" (
    echo [bean] Set BEAN_VELOPACK_SDK_DIR to a valid SDK path, then retry.
    goto :fail
  )
  if not exist "download_velopack_sdk.bat" (
    echo [bean] Missing helper script download_velopack_sdk.bat.
    goto :fail
  )
  echo [bean] Bootstrapping Velopack SDK...
  call download_velopack_sdk.bat
  if errorlevel 1 goto :fail
  if not exist "%SDK_DIR%\include\Velopack.h" (
    echo [bean] Velopack SDK bootstrap did not produce "%SDK_DIR%\include\Velopack.h".
    goto :fail
  )
)

set "BEAN_BUILD_DIR=build-velopack-release"
set "BEAN_DIST_DIR=dist\alpha-release"

set "BEAN_CMAKE_ARGS=-DBEAN_ENABLE_VELOPACK=ON -DBEAN_VELOPACK_SDK_DIR=""%SDK_DIR%"""
if defined BEAN_VELOPACK_UPDATE_URL (
  set "BEAN_CMAKE_ARGS=%BEAN_CMAKE_ARGS% -DBEAN_VELOPACK_UPDATE_URL=""%BEAN_VELOPACK_UPDATE_URL%"""
)

echo [bean] Building Velopack-enabled release...
call build_release_app.bat
if errorlevel 1 goto :fail

echo [bean] Packaging Velopack release...
call package_velopack_release.bat "%PACK_VERSION%"
if errorlevel 1 goto :fail

echo.
echo [bean] Velopack build + package complete.
goto :done

:fail
echo.
echo [bean] Velopack build/package failed.
popd
exit /b 1

:done
popd
exit /b 0
