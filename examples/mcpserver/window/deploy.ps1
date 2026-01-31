# Windows deployment script for window.exe
# Workaround for windeployqt bug with custom Qt libraries
# 
# Issue: windeployqt treats any DLL starting with "Qt6" as an official library
# and tries to find it in the Qt installation, failing if it's actually a custom library.
# 
# Solution: Use --ignore-library-errors flag to skip validation of custom libraries.

param(
    [string]$ExePath = ".\window.exe",
    [string]$QtBinDir = "C:\Qt\6.9.2\msvc2022_64\bin"
)

# Get absolute path
$ExePath = Resolve-Path $ExePath
$ExeDir = Split-Path -Parent $ExePath
$QtBinPath = "$QtBinDir\windeployqt.exe"

if (-not (Test-Path $QtBinPath)) {
    Write-Error "windeployqt not found at: $QtBinPath"
    exit 1
}

Write-Host "Deploying $ExePath"
Write-Host "Working directory: $ExeDir"

# Run windeployqt with --ignore-library-errors to skip validation of custom Qt libraries
Write-Host "Running windeployqt with --ignore-library-errors..."
Write-Host "  (This ignores the error about custom Qt6McpServer* libraries not found in Qt installation)"
& $QtBinPath --ignore-library-errors $ExePath

$result = $LASTEXITCODE

if ($result -eq 0) {
    Write-Host "Deployment successful!"
} else {
    Write-Host "WARNING: Deployment exited with code: $result"
    Write-Host "Check output above for details."
}

exit $result

