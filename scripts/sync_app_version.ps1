param(
    [Parameter(Mandatory = $true)]
    [string]$Version
)

$ErrorActionPreference = "Stop"

if ($Version -notmatch '^(?<core>[0-9]+(?:\.[0-9]+){2})($|[-+])') {
    throw "Invalid version '$Version'. Expected semver, e.g. 0.2.0 or 0.2.0-beta.1."
}

$cmakeVersion = $matches.core

$mainPath = "src/app/main.cpp"
$mainContent = Get-Content $mainPath -Raw
$updatedMain = [regex]::Replace(
    $mainContent,
    'constexpr wchar_t kAppVersion\[\] = L"[^"]+";',
    ('constexpr wchar_t kAppVersion[] = L"' + $Version + '";'),
    1)
if ($updatedMain -eq $mainContent) {
    throw "Could not update app version in $mainPath."
}
[System.IO.File]::WriteAllText($mainPath, $updatedMain, [System.Text.UTF8Encoding]::new($false))

$cmakePath = "CMakeLists.txt"
$cmakeContent = Get-Content $cmakePath -Raw
$updatedCmake = [regex]::Replace(
    $cmakeContent,
    'project\(bean VERSION [^\s\)]+ LANGUAGES CXX\)',
    ('project(bean VERSION ' + $cmakeVersion + ' LANGUAGES CXX)'),
    1)
if ($updatedCmake -eq $cmakeContent) {
    throw "Could not update project version in $cmakePath."
}
[System.IO.File]::WriteAllText($cmakePath, $updatedCmake, [System.Text.UTF8Encoding]::new($false))

Write-Host ("[bean] Version sync complete. UI={0} CMake={1}" -f $Version, $cmakeVersion)
