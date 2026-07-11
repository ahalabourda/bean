@echo off
setlocal
pushd "%~dp0"

set "PACK_ID=%BEAN_VELOPACK_PACK_ID%"
if not defined PACK_ID set "PACK_ID=gg.andrew.bean"

set "PACK_VERSION=%~1"
if not defined PACK_VERSION set "PACK_VERSION=%BEAN_VELOPACK_VERSION%"
if not defined PACK_VERSION (
  echo [bean] Usage: package_velopack_release.bat ^<semver-version^>
  echo [bean] Or set BEAN_VELOPACK_VERSION in your environment.
  goto :fail
)

set "PACK_DIR=dist\alpha-release"
if not exist "%PACK_DIR%\bean.exe" (
  echo [bean] Velopack release payload not found at "%PACK_DIR%".
  echo [bean] Run build_and_package_velopack.bat ^<semver-version^> instead.
  goto :fail
)
if not exist "%PACK_DIR%\velopack_libc.dll" (
  echo [bean] Velopack runtime is missing from "%PACK_DIR%".
  echo [bean] Refusing to package a non-Velopack build.
  echo [bean] Run build_and_package_velopack.bat ^<semver-version^> instead.
  goto :fail
)

where vpk >nul 2>&1
if errorlevel 1 (
  echo [bean] vpk CLI was not found on PATH.
  echo [bean] Install it with: dotnet tool install -g vpk
  goto :fail
)

set "ICON_PATH=assets\icons\bean-256.ico"
set "VPK_ICON_ARG="
if exist "%ICON_PATH%" set "VPK_ICON_ARG=--icon ""%ICON_PATH%"""

echo [bean] Packing Velopack release...
vpk pack --packId "%PACK_ID%" --packVersion "%PACK_VERSION%" --packDir "%PACK_DIR%" --mainExe "bean.exe" --packTitle "Battle Encounter Archival Nexus" %VPK_ICON_ARG%
if errorlevel 1 goto :fail

echo.
echo [bean] Velopack package created in Releases\
goto :done

:fail
echo.
echo [bean] Velopack packaging failed.
popd
exit /b 1

:done
popd
exit /b 0
