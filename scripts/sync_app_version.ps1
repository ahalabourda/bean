param(
    [Parameter(Mandatory = $true)]
    [string]$Version
)

$ErrorActionPreference = "Stop"

if ($Version -notmatch '^(?<core>[0-9]+(?:\.[0-9]+){2})($|[-+])') {
    throw "Invalid version '$Version'. Expected semver, e.g. 0.2.0 or 0.2.0-beta.1."
}

$cmakeVersion = $matches.core

$cmakePath = "CMakeLists.txt"
$cmakeContent = Get-Content $cmakePath -Raw
$cmakePattern = 'project\(bean VERSION [^\s\)]+ LANGUAGES CXX\)'
$cmakeMatch = [regex]::IsMatch($cmakeContent, $cmakePattern)
if (-not $cmakeMatch) {
    throw "Could not find project(bean VERSION ...) in $cmakePath."
}
$updatedCmake = [regex]::Replace(
    $cmakeContent,
    $cmakePattern,
    ('project(bean VERSION ' + $cmakeVersion + ' LANGUAGES CXX)'),
    1)
if ($updatedCmake -ne $cmakeContent) {
    [System.IO.File]::WriteAllText($cmakePath, $updatedCmake, [System.Text.UTF8Encoding]::new($false))
}

Write-Host ("[bean] Version sync complete. App={0} CMake={1}" -f $Version, $cmakeVersion)
