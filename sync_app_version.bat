@echo off
setlocal
pushd "%~dp0"

set "FULL_VERSION=%~1"
if not defined FULL_VERSION set "FULL_VERSION=%BEAN_VELOPACK_VERSION%"
if not defined FULL_VERSION (
  echo [bean] Usage: sync_app_version.bat ^<semver-version^>
  echo [bean] Or set BEAN_VELOPACK_VERSION in your environment.
  goto :fail
)

powershell -NoProfile -ExecutionPolicy Bypass -File "scripts\sync_app_version.ps1" -Version "%FULL_VERSION%"
if errorlevel 1 goto :fail

goto :done

:fail
echo.
echo [bean] Version sync failed.
popd
exit /b 1

:done
popd
exit /b 0
