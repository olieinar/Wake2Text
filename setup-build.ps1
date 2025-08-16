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
    Write-Host "Cleaning build directories..." -ForegroundColor Yellow
    if (Test-Path "build") {
        Remove-Item -Recurse -Force "build"
    }
    if (Test-Path "whisper.cpp\build") {
        Remove-Item -Recurse -Force "whisper.cpp\build"
    }
    Write-Host "Build directories cleaned." -ForegroundColor Green
}

# Ensure submodules are initialized
Write-Host "Checking submodules..." -ForegroundColor Cyan
$submoduleStatus = git submodule status
if ($submoduleStatus -match "^-") {
    Write-Host "Initializing submodules..." -ForegroundColor Yellow
    git submodule update --init --recursive
}

# Build Whisper.cpp first
Write-Host "Building Whisper.cpp..." -ForegroundColor Cyan

# Check if we need to reconfigure whisper.cpp (CUDA setting changed)
$needsReconfigure = $false
if (Test-Path "whisper.cpp\build\CMakeCache.txt") {
    $cacheContent = Get-Content "whisper.cpp\build\CMakeCache.txt" -Raw
    $currentHasCuda = $cacheContent -match "GGML_CUDA:BOOL=ON"
    if ($CUDA -and -not $currentHasCuda) {
        Write-Host "Detected CUDA flag but previous build was CPU-only. Reconfiguring..." -ForegroundColor Yellow
        $needsReconfigure = $true
    } elseif (-not $CUDA -and $currentHasCuda) {
        Write-Host "Detected CPU-only flag but previous build had CUDA. Reconfiguring..." -ForegroundColor Yellow
        $needsReconfigure = $true
    }
}

if ($needsReconfigure) {
    Remove-Item -Recurse -Force "whisper.cpp\build"
}

if (!(Test-Path "whisper.cpp\build")) {
    New-Item -ItemType Directory -Path "whisper.cpp\build" -Force | Out-Null
}

Set-Location "whisper.cpp\build"

# Configure Whisper.cpp
$whisperCmakeArgs = @("..", "-DCMAKE_BUILD_TYPE=$BuildType")
if ($CUDA) {
    Write-Host "Configuring Whisper.cpp with CUDA support..." -ForegroundColor Green
    $whisperCmakeArgs += "-DGGML_CUDA=ON"
} else {
    Write-Host "Configuring Whisper.cpp for CPU-only..." -ForegroundColor Cyan
}

& cmake @whisperCmakeArgs
if ($LASTEXITCODE -ne 0) {
    Write-Error "Whisper.cpp cmake configuration failed!"
    exit 1
}

# Build Whisper.cpp
& cmake --build . --config $BuildType
if ($LASTEXITCODE -ne 0) {
    Write-Error "Whisper.cpp build failed!"
    exit 1
}

# Download Whisper model if it doesn't exist
Set-Location "..\models"
if (!(Test-Path "ggml-large-v3.bin")) {
    Write-Host "Downloading Whisper large-v3 model (~3GB)..." -ForegroundColor Yellow
    Write-Host "This may take a few minutes depending on your internet connection." -ForegroundColor Yellow
    & ".\download-ggml-model.cmd" "large-v3"
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Model download failed!"
        exit 1
    }
    Write-Host "Model downloaded successfully!" -ForegroundColor Green
} else {
    Write-Host "Whisper model already exists, skipping download." -ForegroundColor Green
}

# Navigate back to project root
Set-Location "..\.."

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

# Note: CUDA is handled at the whisper.cpp level, not the main project level
# The main project just links to the whisper libraries

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
