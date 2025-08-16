# Wake2Text

A real-time hotword-activated speech-to-text transcription system that combines snowman hotword detection with OpenAI's Whisper for accurate transcription.

## Features

- **Hotword Detection**: Uses snowman library for efficient hotword detection
- **Real-time Transcription**: Integrates with Whisper.cpp for high-quality speech-to-text
- **Cross-platform Audio**: Supports Windows (WinMM) and Linux (PulseAudio)
- **Multiple Hotword Models**: Supports various hotword models including "hey casper", "alexa", "computer", etc.
- **GPU Acceleration**: Optional CUDA support for faster Whisper inference
- **Hallucination Filtering**: Built-in filtering for common Whisper hallucinations

## Prerequisites

### Windows
- Visual Studio 2022 (or Visual Studio Build Tools)
- CMake 3.16 or higher
- Git with submodules support
- PowerShell (for the automated build script)

### Linux
- GCC or Clang with C++17 support
- CMake 3.16 or higher
- PulseAudio development libraries: `sudo apt-get install libpulse-dev`
- Git with submodules support

## Installation

### Quick Build (Recommended)

#### Windows
Use the automated build script that handles all common issues:
```powershell
# Simple build
.\setup-build.ps1

# Clean build from scratch
.\setup-build.ps1 -Clean

# Build with CUDA (if available)
.\setup-build.ps1 -CUDA

# Debug build
.\setup-build.ps1 -BuildType Debug
```

The automated script:
- ✅ Handles submodule initialization automatically
- ✅ Builds Whisper.cpp with proper configuration
- ✅ Downloads Whisper large-v3 model automatically (~3GB)
- ✅ Creates required OpenBLAS configuration
- ✅ Automatically copies DLLs to correct locations
- ✅ Configures and builds everything correctly

### Manual Build

If you prefer to build manually or need to troubleshoot:

1. **Clone the repository with submodules**:
   ```bash
   git clone --recursive https://github.com/your-username/Wake2Text.git
   cd Wake2Text
   ```

2. **Initialize submodules** (if submodule errors occur):
   ```bash
   git submodule update --init --recursive
   
   # If "not our ref" errors occur, manually clone:
   rm -rf snowman whisper.cpp
   git clone https://github.com/olieinar/snowman.git
   git clone https://github.com/ggml-org/whisper.cpp.git
   ```

3. **Build Whisper.cpp**:
   ```bash
   cd whisper.cpp
   mkdir build && cd build
   
   # CPU-only (recommended)
   cmake .. -DCMAKE_BUILD_TYPE=Release
   cmake --build . --config Release
   
   # Or with CUDA (if you have NVIDIA GPU)
   cmake .. -DGGML_CUDA=ON -DCMAKE_BUILD_TYPE=Release
   cmake --build . --config Release
   
   cd ../..
   ```

4. **Download Whisper models**:
   ```bash
   cd whisper.cpp/models
   
   # Windows
   .\download-ggml-model.cmd large-v3
   
   # Linux/Mac
   ./download-ggml-model.sh large-v3
   
   cd ../..
   ```

5. **Build Wake2Text**:
   
   **Windows:**
   ```powershell
   mkdir build
   cd build
   
   # Create OpenBLAS config (handles dependency issues)
   @"
   add_library(OpenBLAS::OpenBLAS INTERFACE IMPORTED)
   target_include_directories(OpenBLAS::OpenBLAS INTERFACE "`${CMAKE_CURRENT_SOURCE_DIR}/..")
   set(OpenBLAS_FOUND TRUE)
   "@ | Out-File -FilePath "OpenBLASConfig.cmake" -Encoding UTF8
   
   # Configure and build
   cmake .. -G "Visual Studio 17 2022" -DCMAKE_PREFIX_PATH="$PWD"
   cmake --build . --config Release
   ```
   
   **Linux:**
   ```bash
   mkdir build
   cd build
   cmake .. -DCMAKE_BUILD_TYPE=Release
   make -j$(nproc)
   ```

**Note**: The build now automatically copies all required DLLs to the executable directory, eliminating manual DLL copying issues.

## Usage

Run the executable from the build directory:

```bash
# Windows
.\build\Release\wake2text.exe

# Linux
./build/wake2text
```

### Command Line Options

- `--lang=LANG`: Set language (default: "auto", options: "en", "is", "fr", etc.)
- `--gpu`: Enable GPU acceleration (requires CUDA build of Whisper)
- `--ngl=N`: Number of GPU layers to offload (default: 0)
- `--model=PATH`: Path to custom hotword model
- `--quiet` or `-q`: Reduce output verbosity

### Examples

```bash
# Basic usage (Windows)
build\Release\wake2text.exe

# With GPU acceleration
build\Release\wake2text.exe --gpu

# Icelandic transcription with custom GPU layers
build\Release\wake2text.exe --lang=is --ngl=35

# Quiet mode with custom model
build\Release\wake2text.exe --quiet --model=resources/pmdl/custom.pmdl

# Linux examples
./build/wake2text
./build/wake2text --gpu
./build/wake2text --lang=is --ngl=35
```

## How It Works

1. **Initialization**: Loads hotword detection model and initializes audio input
2. **Listening**: Continuously monitors audio for the configured hotword
3. **Activation**: When hotword is detected, starts recording and real-time transcription
4. **Transcription**: Uses Whisper.cpp to transcribe speech in real-time chunks
5. **Output**: Displays transcribed text as you speak
6. **Deactivation**: Stops transcription after detecting silence

## Project Structure

```
Wake2Text/
├── CMakeLists.txt              # Main build configuration
├── cblas.h                     # Minimal CBLAS implementation for Windows
├── src/
│   └── main.cpp               # Main application
├── resources/                  # Hotword models and resources
│   ├── common.res             # Snowman common resources
│   ├── pmdl/                  # Personal hotword models
│   │   └── hey_casper.pmdl    # Default "hey casper" model
│   └── models/                # Universal hotword models
├── snowman/                   # Snowman hotword detection library (submodule)
├── whisper.cpp/               # Whisper.cpp transcription library (submodule)
└── build/                     # Build output directory
```

## Supported Hotwords

The application comes with several pre-trained hotword models:

- **hey casper** (default) - `resources/pmdl/hey_casper.pmdl`
- **alexa** - `resources/alexa/alexa.umdl`
- **computer** - `resources/models/computer.umdl`
- **jarvis** - `resources/models/jarvis.umdl`
- **hey extreme** - `resources/models/hey_extreme.umdl`

## Performance Tips

- **GPU Acceleration**: Use `--gpu` flag if you have an NVIDIA GPU with CUDA support
- **Model Selection**: Large-v3 provides the best accuracy but requires more resources
- **Audio Quality**: Use a good quality microphone for better detection and transcription
- **Environment**: Minimize background noise for optimal performance

## Troubleshooting

### Common Issues

1. **"Could not locate whisper-cli.exe"**:
   - Make sure you built whisper.cpp first
   - Check that the executable exists in `whisper.cpp/build/bin/Release/` or similar

2. **"Required model ggml-large-v3.bin not found"**:
   - Download the model using the whisper.cpp model download script in `whisper.cpp/models/`
   - Verify the model file exists and is ~3GB in size

3. **Application crashes on startup with missing DLL error**:
   - **Solution**: Use the automated build script (`setup-build.ps1`) which handles DLL copying
   - Or manually copy DLLs from `build/bin/Release/` to `build/Release/`
   - Ensure all whisper DLLs are in the same directory as wake2text.exe

4. **OpenBLAS not found during cmake configuration**:
   - **Solution**: The automated build script creates the required OpenBLAS config
   - If building manually, create `build/OpenBLASConfig.cmake` with the OpenBLAS config shown in the manual build section

5. **Submodules are empty after cloning**:
   - **Solution**: 
   ```bash
   git submodule update --init --recursive
   # If that fails with "not our ref" errors:
   rm -rf snowman whisper.cpp
   git clone https://github.com/olieinar/snowman.git
   git clone https://github.com/ggml-org/whisper.cpp.git
   ```

6. **Audio input issues**:
   - **Windows**: Check microphone permissions and default audio device
   - **Linux**: Install PulseAudio development packages and check audio permissions

7. **GPU acceleration not working**:
   - Verify CUDA installation and compatible GPU
   - Rebuild whisper.cpp with CUDA support (`-DGGML_CUDA=ON`)
   - Check that CUDA libraries are in your PATH

### Debug Mode

Set environment variables for debugging:

```bash
# Enable Whisper command debugging
export WS_DEBUG_WHISPER=1

# Enable model path debugging
export WS_DEBUG_MODEL=1
```

## Contributing

1. Fork the repository
2. Create a feature branch: `git checkout -b feature-name`
3. Make your changes and test thoroughly
4. Commit your changes: `git commit -am 'Add feature'`
5. Push to the branch: `git push origin feature-name`
6. Submit a pull request

## Dependencies

- [Snowman](https://github.com/olieinar/snowman) - Hotword detection library
- [Whisper.cpp](https://github.com/ggml-org/whisper.cpp) - Fast C++ implementation of OpenAI's Whisper
- Windows: WinMM (Windows Multimedia API)
- Linux: PulseAudio Simple API

## License

See [LICENSE](LICENSE) file for details.

## Acknowledgments

- OpenAI for the Whisper speech recognition model
- The ggml-org team for the efficient C++ implementation
- Contributors to the snowman hotword detection library
