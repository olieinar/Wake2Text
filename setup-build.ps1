# Wake2Text Build Setup Script
# This script prepares the build environment and handles common issues

param(
    [switch]$Clean,
    [switch]$CUDA,
    [string]$BuildType = "Release"
)

Write-Host "Wake2Text Build Setup" -ForegroundColor Green
Write-Host "====================" -ForegroundColor Green

# Navigate to project root
$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $ProjectRoot

# Clean build if requested
if ($Clean) {
    Write-Host "Cleaning build directory..." -ForegroundColor Yellow
    if (Test-Path "build") {
        Remove-Item -Recurse -Force "build"
    }
}

# Ensure submodules are initialized
Write-Host "Checking submodules..." -ForegroundColor Cyan
$submoduleStatus = git submodule status
if ($submoduleStatus -match "^-") {
    Write-Host "Initializing submodules..." -ForegroundColor Yellow
    git submodule update --init --recursive
}

# Create build directory
if (!(Test-Path "build")) {
    New-Item -ItemType Directory -Path "build" | Out-Null
}
Set-Location "build"

# Create OpenBLAS config if it doesn't exist
$openBlasConfig = @"
# Minimal OpenBLAS config for Wake2Text
# This provides the required OpenBLAS target using the minimal cblas.h implementation

# Create the OpenBLAS target
add_library(OpenBLAS::OpenBLAS INTERFACE IMPORTED)

# Set the include directory to the parent directory (where cblas.h is located)
target_include_directories(OpenBLAS::OpenBLAS INTERFACE "`${CMAKE_CURRENT_SOURCE_DIR}/..")

# Mark the package as found
set(OpenBLAS_FOUND TRUE)
"@

$openBlasConfig | Out-File -FilePath "OpenBLASConfig.cmake" -Encoding UTF8

# Configure cmake
Write-Host "Configuring build..." -ForegroundColor Cyan
$cmakeArgs = @(
    ".."
    "-G", "Visual Studio 17 2022"
    "-DCMAKE_BUILD_TYPE=$BuildType"
    "-DCMAKE_PREFIX_PATH=$PWD"
)

if ($CUDA) {
    Write-Host "CUDA support enabled" -ForegroundColor Green
    # Note: This would require proper CUDA setup
    $cmakeArgs += "-DWHISPER_CUDA=ON"
}

& cmake @cmakeArgs

if ($LASTEXITCODE -ne 0) {
    Write-Error "CMake configuration failed!"
    exit 1
}

# Build
Write-Host "Building project..." -ForegroundColor Cyan
& cmake --build . --config $BuildType

if ($LASTEXITCODE -ne 0) {
    Write-Error "Build failed!"
    exit 1
}

Write-Host "" -ForegroundColor Green
Write-Host "Build completed successfully!" -ForegroundColor Green
Write-Host "Executable location: build\$BuildType\wake2text.exe" -ForegroundColor Yellow
Write-Host "" -ForegroundColor Green
Write-Host "To run the application:" -ForegroundColor Cyan
Write-Host "  .\$BuildType\wake2text.exe" -ForegroundColor White
Write-Host "  .\$BuildType\wake2text.exe --help" -ForegroundColor White
Write-Host ""
