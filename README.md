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

### Linux
- GCC or Clang with C++17 support
- CMake 3.16 or higher
- PulseAudio development libraries: `sudo apt-get install libpulse-dev`
- Git with submodules support

## Installation

1. **Clone the repository with submodules**:
   ```bash
   git clone --recursive https://github.com/your-username/Wake2Text.git
   cd Wake2Text
   ```

2. **Update submodules** (if not cloned recursively):
   ```bash
   git submodule update --init --recursive
   ```

3. **Build Whisper.cpp** (required for transcription):
   ```bash
   # For CPU-only version
   cd whisper.cpp
   mkdir build && cd build
   cmake .. -DCMAKE_BUILD_TYPE=Release
   cmake --build . --config Release
   cd ../..
   
   # For CUDA version (if you have NVIDIA GPU)
   cd whisper.cpp
   mkdir build-cuda && cd build-cuda
   cmake .. -DWHISPER_CUDA=ON -DCMAKE_BUILD_TYPE=Release
   cmake --build . --config Release
   cd ../..
   ```

4. **Download Whisper models**:
   ```bash
   cd whisper.cpp/models
   ./download-ggml-model.sh large-v3
   # Or on Windows:
   ./download-ggml-model.cmd large-v3
   cd ../..
   ```

5. **Build Wake2Text**:
   ```bash
   mkdir build
   cd build
   
   # Windows with Visual Studio
   cmake .. -G "Visual Studio 17 2022"
   cmake --build . --config Release
   
   # Linux
   cmake .. -DCMAKE_BUILD_TYPE=Release
   make -j$(nproc)
   ```

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

# Spanish transcription with custom sensitivity
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
   - Download the model using the whisper.cpp model download script
   - Place it in `models/` or `whisper.cpp/models/` directory

3. **Audio input issues**:
   - Windows: Check microphone permissions and default audio device
   - Linux: Install PulseAudio development packages and check audio permissions

4. **GPU acceleration not working**:
   - Verify CUDA installation and compatible GPU
   - Rebuild whisper.cpp with CUDA support (`-DWHISPER_CUDA=ON`)
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
