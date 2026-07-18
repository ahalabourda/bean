@echo off
setlocal
pushd "%~dp0"

set "DRY_RUN=0"
set "VERSION="

if /I "%~1"=="--dry-run" (
  set "DRY_RUN=1"
  set "VERSION=%~2"
) else (
  set "VERSION=%~1"
  if /I "%~2"=="--dry-run" set "DRY_RUN=1"
)

if not defined VERSION set "VERSION=%BEAN_VELOPACK_VERSION%"
if not defined VERSION (
  echo [bean] Usage: ship_release.bat ^<semver-version^> [--dry-run]
  echo [bean] Example: ship_release.bat 0.2.3 --dry-run
  goto :fail
)

git rev-parse --is-inside-work-tree >nul 2>&1
if errorlevel 1 (
  echo [bean] This script must run inside a git repository.
  goto :fail
)

set "HAS_PENDING_CHANGES="
for /f %%I in ('git status --porcelain') do (
  set "HAS_PENDING_CHANGES=1"
)
if defined HAS_PENDING_CHANGES (
  if "%DRY_RUN%"=="1" (
    echo [bean] Warning: working tree is not clean. Dry run will continue.
  ) else (
    echo [bean] Working tree is not clean.
    echo [bean] Commit or stash your current changes before shipping a release.
    goto :fail
  )
)

git rev-parse -q --verify "refs/tags/v%VERSION%" >nul 2>&1
if not errorlevel 1 (
  echo [bean] Tag v%VERSION% already exists locally.
  goto :fail
)

git ls-remote --exit-code --tags origin "refs/tags/v%VERSION%" >nul 2>&1
if not errorlevel 1 (
  echo [bean] Tag v%VERSION% already exists on origin.
  goto :fail
)

if "%DRY_RUN%"=="1" (
  echo [bean] Dry run mode enabled.
  echo [bean] Would run:
  echo [bean]   sync_app_version.bat %VERSION%
  echo [bean]   git add CMakeLists.txt src/app/bean_version.h.in
  echo [bean]   git commit -m "release: %VERSION%"
  echo [bean]   git push origin HEAD
  echo [bean]   git tag -a v%VERSION% -m "Release v%VERSION%"
  echo [bean]   git push origin v%VERSION%
  goto :done
)

echo [bean] Syncing app version to %VERSION%...
call sync_app_version.bat "%VERSION%"
if errorlevel 1 goto :fail

git add "CMakeLists.txt" "src/app/bean_version.h.in"
git diff --cached --quiet
if not errorlevel 1 (
  echo [bean] Version files already match %VERSION%.
  echo [bean] No release commit was created.
  goto :fail
)

echo [bean] Creating release commit...
git commit -m "release: %VERSION%"
if errorlevel 1 goto :fail

echo [bean] Pushing release commit...
git push origin HEAD
if errorlevel 1 goto :fail

echo [bean] Creating and pushing tag v%VERSION%...
git tag -a "v%VERSION%" -m "Release v%VERSION%"
if errorlevel 1 goto :fail
git push origin "v%VERSION%"
if errorlevel 1 goto :fail

echo.
echo [bean] Release shipped successfully.
echo [bean] GitHub Actions release workflow should now run for tag v%VERSION%.
goto :done

:fail
echo.
echo [bean] Release ship failed.
popd
exit /b 1

:done
popd
exit /b 0
