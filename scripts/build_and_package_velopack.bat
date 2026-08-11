@echo off
setlocal
pushd "%~dp0.."

set "VERSION=%~1"
if not defined VERSION set "VERSION=%BEAN_VELOPACK_VERSION%"
if not defined VERSION (
  echo [bean] Usage: build_and_package_velopack.bat ^<semver-version^>
  goto :fail
)

set "SDK_DIR=%BEAN_VELOPACK_SDK_DIR%"
if not defined SDK_DIR set "SDK_DIR=%CD%\tools\velopack-sdk"
if not exist "%SDK_DIR%\include\Velopack.h" (
  echo [bean] Velopack SDK not found at "%SDK_DIR%".
  echo [bean] Run scripts\download_velopack_sdk.bat first.
  goto :fail
)

echo [bean] Configuring Velopack build...
cmake -S . -B build-velopack -DBEAN_ENABLE_LIBOBS=ON -DBEAN_ENABLE_VELOPACK=ON -DBEAN_VELOPACK_SDK_DIR="%SDK_DIR%" -DBEAN_VELOPACK_UPDATE_URL="%BEAN_VELOPACK_UPDATE_URL%"
if errorlevel 1 goto :fail

echo [bean] Building application...
cmake --build build-velopack --config Release --target bean_app
if errorlevel 1 goto :fail

set "PACK_DIR=dist\alpha-release"
if exist "%PACK_DIR%" rmdir /S /Q "%PACK_DIR%"
mkdir "%PACK_DIR%"
copy /Y "build-velopack\Release\bean.exe" "%PACK_DIR%\bean.exe" >nul
if errorlevel 1 goto :fail
copy /Y "build-velopack\Release\velopack_libc.dll" "%PACK_DIR%\velopack_libc.dll" >nul
if errorlevel 1 goto :fail
xcopy /E /I /Y "build-velopack\Release\assets" "%PACK_DIR%\assets" >nul
if errorlevel 1 goto :fail

call "%~dp0package_velopack_release.bat" "%VERSION%"
if errorlevel 1 goto :fail

goto :done

:fail
echo.
echo [bean] Velopack build/package failed.
popd
exit /b 1

:done
echo.
echo [bean] Velopack build/package completed for %VERSION%.
popd
exit /b 0
@echo off
setlocal
pushd "%~dp0.."

set "PACK_VERSION=%~1"
if not defined PACK_VERSION set "PACK_VERSION=%BEAN_VELOPACK_VERSION%"
if not defined PACK_VERSION (
  echo [bean] Usage: build_and_package_velopack.bat ^<semver-version^>
  echo [bean] Or set BEAN_VELOPACK_VERSION in your environment.
  goto :fail
)

if not exist "%~dp0sync_app_version.bat" (
  echo [bean] Missing helper script sync_app_version.bat.
  goto :fail
)
echo [bean] Syncing app version to %PACK_VERSION%...
call "%~dp0sync_app_version.bat" "%PACK_VERSION%"
if errorlevel 1 goto :fail

set "SDK_DIR=%BEAN_VELOPACK_SDK_DIR%"
if not defined SDK_DIR set "SDK_DIR=tools\velopack-sdk"
if not exist "%SDK_DIR%\include\Velopack.h" (
  echo [bean] Velopack SDK not found at "%SDK_DIR%".
  if /I not "%SDK_DIR%"=="tools\velopack-sdk" (
    echo [bean] Set BEAN_VELOPACK_SDK_DIR to a valid SDK path, then retry.
    goto :fail
  )
  if not exist "%~dp0download_velopack_sdk.bat" (
    echo [bean] Missing helper script download_velopack_sdk.bat.
    goto :fail
  )
  echo [bean] Bootstrapping Velopack SDK...
  call "%~dp0download_velopack_sdk.bat"
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
call "%~dp0build_release_app.bat"
if errorlevel 1 goto :fail

echo [bean] Packaging Velopack release...
call "%~dp0package_velopack_release.bat" "%PACK_VERSION%"
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
