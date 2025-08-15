#include "helper.h"
#include <iostream>
#include <algorithm>
#include <chrono>
#include <thread>
#include <sstream>
#include <fstream>
#include <cstdlib>
#include <ctime>
#include <cmath>
#include "snowboy-detect.h"
#include "pulseaudio.hh"

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#include <filesystem>
#else
#include <sys/wait.h>
#endif

namespace pa = pulseaudio::pa;

class WhisperStreamingTranscriber {
private:
    std::string root;
    std::string model;
    std::string hotword;
    pa::simple_record_stream* audio_in;
    snowboy::SnowboyDetect* detector;
    snowboy::SnowboyVad* vad;
    
    // Whisper integration
    std::string whisper_model_path;
    std::string whisper_exe_path;
    std::string lang_code = "en";
    int ngl_layers = 0; // number of layers to offload to GPU (-ngl); 0 = CPU only
    
    // Real-time transcription settings
    std::vector<short> audio_buffer;
    bool is_listening = false;
    int silence_counter = 0;
    int speech_counter = 0;
    const int SILENCE_THRESHOLD = 30; // ~2.0 seconds of silence to stop
    const int MIN_SPEECH_LENGTH = 8000; // 0.5 second minimum before starting transcription
    const int TRANSCRIPTION_CHUNK_SIZE = 48000; // 3.0 seconds of audio for each transcription chunk
    
    std::string current_transcription;
    bool transcription_started = false;
    int chunk_count = 0; // Track chunk number for overlap adjustment
    bool quiet_mode = false; // Flag to reduce output verbosity
    
    // Helper function to save WAV file
    void saveWavFile(const std::string& filename, const std::vector<short>& audio_data) {
        FILE* f = fopen(filename.c_str(), "wb");
        if (!f) return;
        
        // Write WAV header
        struct WavHeader {
            char riff[4] = {'R', 'I', 'F', 'F'};
            uint32_t file_size;
            char wave[4] = {'W', 'A', 'V', 'E'};
            char fmt[4] = {'f', 'm', 't', ' '};
            uint32_t fmt_size = 16;
            uint16_t audio_format = 1; // PCM
            uint16_t num_channels = 1;
            uint32_t sample_rate = 16000;
            uint32_t byte_rate = 32000; // sample_rate * num_channels * bits_per_sample/8
            uint16_t block_align = 2;
            uint16_t bits_per_sample = 16;
            char data[4] = {'d', 'a', 't', 'a'};
            uint32_t data_size;
        } header;
        
        header.data_size = audio_data.size() * sizeof(short);
        header.file_size = header.data_size + 44 - 8;
        
        fwrite(&header, sizeof(header), 1, f);
        fwrite(audio_data.data(), sizeof(short), audio_data.size(), f);
        fclose(f);
    }
    
public:
    WhisperStreamingTranscriber(const std::string& model_path = "", const std::string& language = "en", int ngl = 0, bool quiet = false) {
        ngl_layers = ngl;
        lang_code = language;
        quiet_mode = quiet;
        // Build absolute base path from executable location to avoid working-dir issues
#ifdef _WIN32
        char module_path[MAX_PATH];
        DWORD len = GetModuleFileNameA(NULL, module_path, MAX_PATH);
        std::filesystem::path exe_dir = std::filesystem::path(module_path).parent_path();
#else
        std::filesystem::path exe_dir = std::filesystem::current_path();
#endif
        // Project root assumed to be exe_dir/.. if installed in build/apps/Release, but we will resolve relative to exe_dir
        root = ""; // not used now for paths below

        // Resolve project base using detect_project_root() to avoid ambiguity
        std::filesystem::path base = std::filesystem::path(detect_project_root());

        std::filesystem::path default_model = base / "resources" / "pmdl" / "hey_casper.pmdl";
        // Prefer CUDA build of whisper-cli if present
        std::filesystem::path cuda_release = base / "whisper.cpp" / "build-cuda" / "bin" / "Release" / "whisper-cli.exe";
        std::filesystem::path cuda_debug   = base / "whisper.cpp" / "build-cuda" / "bin" / "Debug"   / "whisper-cli.exe";
        std::filesystem::path cuda_plain   = base / "whisper.cpp" / "build-cuda" / "bin"             / "whisper-cli.exe";
        std::filesystem::path msvc_release = base / "whisper.cpp" / "build"      / "bin" / "Release" / "whisper-cli.exe";
        std::filesystem::path msvc_debug   = base / "whisper.cpp" / "build"      / "bin" / "Debug"   / "whisper-cli.exe";
        std::filesystem::path msvc_plain   = base / "whisper.cpp" / "build"      / "bin"             / "whisper-cli.exe";
        // Only use large-v3 model
        std::filesystem::path large_v3_project = base / "models" / "ggml-large-v3.bin";
        std::filesystem::path large_v3_whisper = base / "whisper.cpp" / "models" / "ggml-large-v3.bin";

        model = model_path.empty() ? default_model.string() : model_path;
        // Resolve whisper-cli path candidates in order of preference
        std::vector<std::filesystem::path> wcli_candidates = {
            cuda_release, cuda_debug, cuda_plain,
            msvc_release, msvc_debug, msvc_plain
        };
        for (const auto &p : wcli_candidates) {
            if (std::filesystem::exists(p)) { whisper_exe_path = p.string(); break; }
        }
        if (whisper_exe_path.empty()) {
            throw std::runtime_error("Could not locate whisper-cli.exe. Build it first (e.g., build-cuda Release).");
        }
        
        // Test if the selected whisper-cli actually works (quick DLL test)
        std::string test_cmd = "\"" + whisper_exe_path + "\" --help >nul 2>&1";
        bool using_cpu_fallback = false;
#ifdef _WIN32
        STARTUPINFOA si{};
        PROCESS_INFORMATION pi{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        
        BOOL test_ok = CreateProcessA(NULL, (char*)test_cmd.c_str(), NULL, NULL, FALSE, 
                                     CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
        if (test_ok) {
            WaitForSingleObject(pi.hProcess, 2000); // 2 second timeout
            DWORD exit_code = 1;
            GetExitCodeProcess(pi.hProcess, &exit_code);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            
            // If CUDA version failed (missing DLLs), try CPU version instead
            if (exit_code == 3221225781 || exit_code == 3221225515) { // DLL errors
                std::cout << "[WARN] CUDA whisper-cli failed (exit code " << exit_code << "), trying CPU version..." << std::endl;
                // Try CPU versions
                std::vector<std::filesystem::path> cpu_candidates = { msvc_release, msvc_debug, msvc_plain };
                for (const auto &p : cpu_candidates) {
                    if (std::filesystem::exists(p)) {
                        whisper_exe_path = p.string();
                        using_cpu_fallback = true;
                        std::cout << "[INFO] Switched to CPU whisper-cli: " << whisper_exe_path << std::endl;
                        break;
                    }
                }
            }
        }
#endif
        
        // If we fell back to CPU version, disable GPU layers since CPU version doesn't support -ngl
        if (using_cpu_fallback && ngl_layers > 0) {
            std::cout << "[INFO] Disabling GPU offload (-ngl) as CPU whisper-cli doesn't support it." << std::endl;
            ngl_layers = 0;
        }

        // Resolve model path: prefer project-root models/ggml-large-v3.bin, then whisper.cpp/models/ggml-large-v3.bin
        if (std::filesystem::exists(large_v3_project)) {
            whisper_model_path = std::filesystem::absolute(large_v3_project).string();
        } else if (std::filesystem::exists(large_v3_whisper)) {
            whisper_model_path = std::filesystem::absolute(large_v3_whisper).string();
        } else {
            std::string msg = "Required model ggml-large-v3.bin not found. Checked:\n  " + (large_v3_project.string()) + "\n  " + (large_v3_whisper.string()) +
                              "\nYou can download it with: whisper.cpp\\models\\download-ggml-model.cmd large-v3";
            throw std::runtime_error(msg);
        }
        
        // whisper_exe_path already resolved from candidates above

        // Normalize separators to backslashes on Windows for cmd.exe
#ifdef _WIN32
        auto to_win_path = [](std::string s) {
            for (auto &c : s) if (c == '/') c = '\\';
            return s;
        };
        whisper_exe_path = to_win_path(whisper_exe_path);
        whisper_model_path = to_win_path(whisper_model_path);
        model = to_win_path(model);
        // Ensure absolute model path (in case earlier steps produced relative on some platforms)
        if (!std::filesystem::path(whisper_model_path).is_absolute()) {
            whisper_model_path = std::filesystem::absolute(whisper_model_path).string();
            whisper_model_path = to_win_path(whisper_model_path);
        }
#endif
        
        // Determine hotword name from model file
        hotword = "unknown";
        if (model.find("computer.umdl") != std::string::npos) hotword = "computer";
        else if (model.find("jarvis.umdl") != std::string::npos) hotword = "jarvis";
        else if (model.find("hey_extreme.umdl") != std::string::npos) hotword = "hey extreme";
        else if (model.find("alexa.umdl") != std::string::npos) hotword = "alexa";
        else if (model.find("hey_casper.pmdl") != std::string::npos) hotword = "hey casper";
        else if (model.find(".pmdl") != std::string::npos) {
            // Extract filename from path for .pmdl files
            size_t lastSlash = model.find_last_of("/\\");
            size_t lastDot = model.find_last_of(".");
            if (lastSlash != std::string::npos && lastDot != std::string::npos) {
                hotword = model.substr(lastSlash + 1, lastDot - lastSlash - 1);
                std::replace(hotword.begin(), hotword.end(), '_', ' ');
            }
        }
        
        // Initialize audio and detection
        audio_in = new pa::simple_record_stream("Whisper Streaming Transcriber");
        detector = new snowboy::SnowboyDetect(root + "resources/common.res", model);
        vad = new snowboy::SnowboyVad(root + "resources/common.res");
        
        // Configure detector for better fast speech detection
        detector->SetSensitivity("0.45"); // Lower threshold = more sensitive
        detector->SetAudioGain(1.5); // Higher gain for better detection
        detector->ApplyFrontend(true);
        
        if (!quiet_mode) {
            std::cout << "[init] Whisper Streaming Transcriber initialized" << std::endl;
            std::cout << "Hotword: '" << hotword << "'" << std::endl;
            std::cout << "Model: " << model << std::endl;
            std::cout << "Whisper executable: " << whisper_exe_path << std::endl;
            std::cout << "Whisper model: " << whisper_model_path << std::endl;
            std::cout << "Language: " << lang_code << std::endl;
            if (ngl_layers > 0) {
                std::cout << "GPU offload: enabled (CUDA build)" << std::endl;
            } else {
                std::cout << "GPU offload: disabled (CPU mode)" << std::endl;
            }
        }
        
        // Verify files exist
        if (!std::filesystem::exists(whisper_exe_path)) {
            std::cout << "[ERROR] Whisper executable not found: " << whisper_exe_path << std::endl;
        }
        if (!std::filesystem::exists(whisper_model_path)) {
            std::cout << "[ERROR] Whisper model not found: " << whisper_model_path << std::endl;
        }

        // Optional: write selected model path to a file if WS_DEBUG_MODEL is set
        if (std::getenv("WS_DEBUG_MODEL")) {
            try {
                // Write next to the executable, which is exe_dir captured above
#ifdef _WIN32
                char module_path_dbg[MAX_PATH];
                GetModuleFileNameA(NULL, module_path_dbg, MAX_PATH);
                std::filesystem::path exe_dir_dbg = std::filesystem::path(module_path_dbg).parent_path();
#else
                std::filesystem::path exe_dir_dbg = std::filesystem::current_path();
#endif
                auto out_path = exe_dir_dbg / "whisper_model_selected.txt";
                FILE* outf = fopen(out_path.string().c_str(), "wb");
                if (outf) {
                    fwrite(whisper_model_path.c_str(), 1, whisper_model_path.size(), outf);
                    fclose(outf);
                }
            } catch (...) {
                // ignore
            }
        }
    }
    
    ~WhisperStreamingTranscriber() {
        delete audio_in;
        delete detector;
        delete vad;
    }
    
    // Real transcription with whisper.cpp
    std::string transcribeWithWhisper(const std::vector<short>& audio_chunk) {
        // Create temporary filename with timestamp
        auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::string temp_file = "temp_chunk_" + std::to_string(timestamp) + ".wav";
        
        // Save chunk to temporary WAV file
        saveWavFile(temp_file, audio_chunk);
        
        // Build whisper command with quality improvements
        std::string command = "\"" + whisper_exe_path + "\" -l " + lang_code + " -m \"" + whisper_model_path + "\"";
        // Note: CUDA build enables GPU by default, CPU build doesn't support -ngl
        // Only add --no-gpu if we want to force CPU-only mode
        if (ngl_layers == 0) {
            // Force CPU mode if ngl_layers is 0
            command += " --no-gpu";
        }
        // Add quality parameters for better accuracy
        command += " --best-of 5 --beam-size 5";
        // Reduce no-speech threshold for better detection of quiet speech
        command += " --no-speech-thold 0.3";
        // Improve word-level accuracy
        command += " --word-thold 0.005";
        // Otherwise let CUDA build use GPU by default
        command += " -f \"" + temp_file + "\"";
        
        bool debug_whisper = (std::getenv("WS_DEBUG_WHISPER") != nullptr);
            if (debug_whisper && !quiet_mode) {
                std::cout << "\n[cmd] " << command << std::endl;
            }
            if (!quiet_mode) {
                std::cout << "[proc] " << std::flush;
            }
        
        std::string result = "";

#ifdef _WIN32
        // Use CreateProcess to avoid cmd.exe parsing issues and capture stdout/stderr via pipes
        HANDLE hRead = NULL, hWrite = NULL;
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(SECURITY_ATTRIBUTES);
        sa.bInheritHandle = TRUE;
        sa.lpSecurityDescriptor = NULL;
        if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
            DWORD err = GetLastError();
            std::cout << "\n[ERROR] CreatePipe failed: " << err << std::endl;
            return "[CreatePipe failed]";
        } else {
            // Ensure the read handle is not inherited
            SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

            STARTUPINFOA si{};
            PROCESS_INFORMATION pi{};
            si.cb = sizeof(si);
            si.dwFlags |= STARTF_USESTDHANDLES;
            si.hStdInput = NULL;
            si.hStdOutput = hWrite;
            si.hStdError = hWrite; // capture stderr too

            // Build command line for CreateProcess (consistent with command above)
            std::string cmdLine = command;
            
            // Add CUDA DLLs to PATH for CUDA build
            char* existing_path = nullptr;
            size_t path_len = 0;
            std::string new_path_env;
            if (_dupenv_s(&existing_path, &path_len, "PATH") == 0 && existing_path) {
                new_path_env = "PATH=C:\\Program Files\\NVIDIA GPU Computing Toolkit\\CUDA\\v13.0\\bin\\x64;" + std::string(existing_path);
                free(existing_path);
            } else {
                new_path_env = "PATH=C:\\Program Files\\NVIDIA GPU Computing Toolkit\\CUDA\\v13.0\\bin\\x64";
            }
            
            // Create environment block with CUDA path
            std::vector<char> env_block;
            env_block.insert(env_block.end(), new_path_env.begin(), new_path_env.end());
            env_block.push_back('\0');
            env_block.push_back('\0'); // Double null terminator
            
            // CreateProcessA can take application name separately; we pass NULL and full cmdline
            BOOL ok = CreateProcessA(
                NULL,
                cmdLine.data(), // modifiable buffer per API
                NULL, NULL, TRUE,
                CREATE_NO_WINDOW,
                env_block.data(), NULL, // Use our custom environment
                &si, &pi
            );

            // Parent doesn't need the write end
            CloseHandle(hWrite);

            if (ok) {
                if (debug_whisper && !quiet_mode) {
                    std::cout << "\n[DEBUG] Process created successfully, PID: " << pi.dwProcessId << std::endl;
                }
                // Read child's output and parse lines incrementally to echo immediately
                char buf[2048];
                DWORD bytesRead = 0;
                std::string carry;
                auto trim = [](std::string& s) {
                    size_t start = s.find_first_not_of(" \t\n\r");
                    size_t end = s.find_last_not_of(" \t\n\r");
                    if (start == std::string::npos) { s.clear(); return; }
                    s = s.substr(start, end - start + 1);
                };
                auto handle_line = [&](std::string line) {
                    if (debug_whisper && !quiet_mode) {
                        std::cout << "[whisper] " << line << "\n";
                    }
                    trim(line);
                    if (line.empty()) return;
                    // Timestamped line
                    if (line.size() > 1 && line[0] == '[') {
                        size_t rb = line.find(']');
                        if (rb != std::string::npos && rb + 1 < line.size()) {
                            std::string after = line.substr(rb + 1);
                            trim(after);
                            if (!after.empty()) {
                                // Check for hallucination before echoing
                                std::string lower_text = after;
                                std::transform(lower_text.begin(), lower_text.end(), lower_text.begin(), ::tolower);
                                
                                bool is_hallucination = false;
                                // Quick hallucination check
                                std::vector<std::string> hallucinations = {
                                    "υπότιτλοι", "authorwave", "subtitles", "subtitle", "closed captions",
                                    "captioning", "transcription", "transcript", "audio", "music",
                                    "[music]", "[sound]", "[noise]", "[silence]", "[inaudible]",
                                    "thank you", "thanks for watching", "subscribe", "like and subscribe",
                                    "www.", ".com", "http", "https",
                                    "undertekster", "ai-media", "ai media", "undertekst", "tekster",
                                    "untertitel", "sous-titres", "legendas", "sottotitoli"
                                };
                                
                                for (const auto& halluc : hallucinations) {
                                    if (lower_text.find(halluc) != std::string::npos) {
                                        is_hallucination = true;
                                        break;
                                    }
                                }
                                
                                // Additional check for standalone hallucinations
                                if (!is_hallucination) {
                                    std::string trimmed = lower_text;
                                    trimmed.erase(std::remove_if(trimmed.begin(), trimmed.end(), 
                                        [](char c) { return c == '.' || c == ',' || c == '!' || c == '?' || c == ' ' || c == '\t' || c == '\n' || c == '\r'; }), 
                                        trimmed.end());
                                    
                                    std::vector<std::string> standalone_hallucinations = {
                                        "thankyou", "thankyouforwatching", "thanks", "thanksforwatching",
                                        "subscribe", "likeandsubscribe", "pleasesubscribe"
                                    };
                                    
                                    for (const auto& standalone : standalone_hallucinations) {
                                        if (trimmed == standalone) {
                                            is_hallucination = true;
                                            break;
                                        }
                                    }
                                }
                                
                                if (is_hallucination) {
                                    if (!quiet_mode) {
                                        std::cout << "[filtered: " << after << "] " << std::flush;
                                    }
                                } else {
                                    // Echo immediately only if not hallucination
                                    std::cout << after << " " << std::flush;
                                    if (!result.empty()) result += ' ';
                                    result += after;
                                }
                            }
                            return;
                        }
                    }
                    // Filter noise - expanded list for whisper.cpp output
                    static const char* noise_prefixes[] = {
                        "system_info:", "whisper_print_timings:", "main:", "ggml:", "whisper:", "memcpy(", "AVX",
                        "whisper_init_", "whisper_model_", "whisper_backend_", "whisper_full_",
                        "load time", "fallbacks", "mel time", "sample time", "encode time", "decode time",
                        "batchd time", "prompt time", "total time", "auto-detected language:",
                        "processing '", "threads", "processors", "beams", "lang =", "task =", "timestamps =",
                        "ggml_cuda_init:", "Device 0:", "compute capability", "VMM:", "GGML_CUDA_FORCE",
                        "whisper_init_from_file", "use gpu", "flash attn", "gpu_device", "dtw", "devices",
                        "backends", "whisper_model_load:", "n_vocab", "n_audio", "n_text", "n_mels", "ftype",
                        "qntvr", "type", "adding", "extra tokens", "n_langs", "CUDA0 total size",
                        "model size", "whisper_backend_init_gpu:", "using CUDA", "whisper_init_state:",
                        "kv self size", "kv cross size", "kv pad size", "compute buffer", "WHISPER :",
                        "CPU :", "SSE3", "SSSE3", "AVX", "FMA", "AVX512", "OPENMP", "REPACK"
                    };
                    for (auto p : noise_prefixes) {
                        if (line.rfind(p, 0) == 0) return;
                    }
                    // Echo fallback line
                    if (!quiet_mode) {
                        std::cout << line << " " << std::flush;
                    }
                    if (!result.empty()) result += ' ';
                    result += line;
                };
                for (;;) {
                    BOOL r = ReadFile(hRead, buf, sizeof(buf)-1, &bytesRead, NULL);
                    if (!r || bytesRead == 0) break;
                    buf[bytesRead] = '\0';
                    carry.append(buf);
                    size_t pos = 0;
                    while (true) {
                        size_t nl = carry.find('\n', pos);
                        if (nl == std::string::npos) {
                            // Keep remainder
                            carry = carry.substr(pos);
                            break;
                        }
                        std::string line = carry.substr(pos, nl - pos);
                        handle_line(line);
                        pos = nl + 1;
                    }
                }
                // Trailing partial
                if (!carry.empty()) handle_line(carry);

                // Wait for process to finish
                DWORD exit_code = 0;
                WaitForSingleObject(pi.hProcess, INFINITE);
                GetExitCodeProcess(pi.hProcess, &exit_code);
                if (debug_whisper && !quiet_mode) {
                    std::cout << "\n[DEBUG] Process finished with exit code: " << exit_code << std::endl;
                }
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                CloseHandle(hRead);
            } else {
                DWORD err = GetLastError();
                std::cout << "\n[ERROR] CreateProcess failed: " << err << std::endl;
                std::cout << "[ERROR] Command was: " << cmdLine << std::endl;
                std::cout << "[ERROR] Temp file: " << temp_file << std::endl;
                CloseHandle(hRead);
                return "[CreateProcess failed: " + std::to_string(err) + "]";
            }
        }
#else
        // POSIX fallback: popen
        FILE* pipe = _popen(command.c_str(), "r");
        if (pipe) {
            char buffer[1024];
            while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                std::string line(buffer);
                if (debug_whisper) {
                    std::cout << "[whisper] " << line;
                }
                auto trim = [](std::string& s) {
                    size_t start = s.find_first_not_of(" \t\n\r");
                    size_t end = s.find_last_not_of(" \t\n\r");
                    if (start == std::string::npos) { s.clear(); return; }
                    s = s.substr(start, end - start + 1);
                };
                trim(line);
                if (line.empty()) continue;
                if (line.size() > 1 && line[0] == '[') {
                    size_t rb = line.find(']');
                    if (rb != std::string::npos && rb + 1 < line.size()) {
                        std::string after = line.substr(rb + 1);
                        trim(after);
                        if (!after.empty()) {
                            if (!result.empty()) result += ' ';
                            result += after;
                        }
                        continue;
                    }
                }
                static const char* noise_prefixes[] = {
                    "system_info:", "whisper_print_timings:", "main:", "ggml:", "whisper:", "memcpy(", "AVX",
                    "whisper_init_", "whisper_model_", "whisper_backend_", "whisper_full_",
                    "load time", "fallbacks", "mel time", "sample time", "encode time", "decode time",
                    "batchd time", "prompt time", "total time", "auto-detected language:",
                    "processing '", "threads", "processors", "beams", "lang =", "task =", "timestamps ="
                };
                bool is_noise = false;
                for (auto p : noise_prefixes) {
                    if (line.rfind(p, 0) == 0) { is_noise = true; break; }
                }
                if (!is_noise) {
                    if (!result.empty()) result += ' ';
                    result += line;
                }
            }
            _pclose(pipe);
        }
#endif
        
        // Cleanup temporary file
        std::remove(temp_file.c_str());
        
        // Check if we got any output
        if (result.empty()) {
            std::cout << "\n[WARNING] No output from whisper-cli. Command was:\n  " << command << std::endl;
            std::cout << "[WARNING] Try running this command manually to see the error." << std::endl;
        }
        
        // Final trim
        if (!result.empty()) {
            size_t start = result.find_first_not_of(' ');
            size_t end = result.find_last_not_of(' ');
            if (start == std::string::npos) result.clear();
            else result = result.substr(start, end - start + 1);
        }
        return result;
    }
    
    // Helper function to check if audio chunk has enough speech content
    bool hasSubstantialSpeech(const std::vector<short>& audio_chunk) {
        if (audio_chunk.empty()) return false;
        
        // Calculate RMS (Root Mean Square) to measure audio energy
        long long sum_squares = 0;
        for (short sample : audio_chunk) {
            sum_squares += (long long)sample * sample;
        }
        double rms = sqrt((double)sum_squares / audio_chunk.size());
        
        // Extremely permissive RMS threshold - only filter out completely silent audio
        const double MIN_RMS_THRESHOLD = 50.0; // Very low threshold for near-silence
        
        if (rms < MIN_RMS_THRESHOLD) {
            if (!quiet_mode) {
                std::cout << "[near silence: RMS=" << (int)rms << "] " << std::flush;
            }
            return false;
        }
        
        // Count samples above a very low threshold
        int speech_samples = 0;
        const short SPEECH_THRESHOLD = 200; // Very low threshold - any audio activity
        for (short sample : audio_chunk) {
            if (abs(sample) > SPEECH_THRESHOLD) {
                speech_samples++;
            }
        }
        
        // Require at least 0.5% of samples to have any audio activity (extremely permissive)
        double speech_ratio = (double)speech_samples / audio_chunk.size();
        if (speech_ratio < 0.005) {
            if (!quiet_mode) {
                std::cout << "[no audio activity: " << (int)(speech_ratio * 1000) << "‰] " << std::flush;
            }
            return false;
        }
        
        if (!quiet_mode) {
            std::cout << "[audio OK: RMS=" << (int)rms << ", activity=" << (int)(speech_ratio * 100) << "%] " << std::flush;
        }
        return true;
    }
    
    void processAudioChunk() {
        if (audio_buffer.size() >= TRANSCRIPTION_CHUNK_SIZE) {
            // Extract chunk for transcription
            std::vector<short> chunk(audio_buffer.begin(), 
                                   audio_buffer.begin() + TRANSCRIPTION_CHUNK_SIZE);
            
            // Only filter out truly problematic audio (very permissive thresholds)
            if (!hasSubstantialSpeech(chunk)) {
                if (!quiet_mode) {
                    std::cout << "[skipping chunk - insufficient speech] " << std::flush;
                }
                int overlap = TRANSCRIPTION_CHUNK_SIZE / 4;
                audio_buffer.erase(audio_buffer.begin(), 
                                 audio_buffer.begin() + TRANSCRIPTION_CHUNK_SIZE - overlap);
                return;
            }
            
            // Call whisper.cpp for real transcription
            std::string transcribed_text = transcribeWithWhisper(chunk);
            
            // Filter out common Whisper hallucinations
            if (!transcribed_text.empty()) {
                // Check for common hallucinations
                std::string lower_text = transcribed_text;
                std::transform(lower_text.begin(), lower_text.end(), lower_text.begin(), ::tolower);
                
                // List of common Whisper hallucinations to filter out
                std::vector<std::string> hallucinations = {
                    "υπότιτλοι", "authorwave", "subtitles", "subtitle", "closed captions",
                    "captioning", "transcription", "transcript", "audio", "music",
                    "[music]", "[sound]", "[noise]", "[silence]", "[inaudible]",
                    "thank you", "thanks for watching", "subscribe", "like and subscribe",
                    "www.", ".com", "http", "https",
                    "undertekster", "ai-media", "ai media", "undertekst", "tekster",
                    "untertitel", "sous-titres", "legendas", "sottotitoli"
                };
                
                bool is_hallucination = false;
                for (const auto& halluc : hallucinations) {
                    if (lower_text.find(halluc) != std::string::npos) {
                        if (!quiet_mode) {
                            std::cout << "[filtered hallucination: " << transcribed_text << "] " << std::flush;
                        }
                        is_hallucination = true;
                        break;
                    }
                }
                
                // Additional check for common standalone hallucination words
                if (!is_hallucination) {
                    // Check if the entire transcription is just a common hallucination phrase
                    std::string trimmed = lower_text;
                    // Remove common punctuation and whitespace
                    trimmed.erase(std::remove_if(trimmed.begin(), trimmed.end(), 
                        [](char c) { return c == '.' || c == ',' || c == '!' || c == '?' || c == ' ' || c == '\t' || c == '\n' || c == '\r'; }), 
                        trimmed.end());
                    
                    std::vector<std::string> standalone_hallucinations = {
                        "thankyou", "thankyouforwatching", "thanks", "thanksforwatching",
                        "subscribe", "likeandsubscribe", "pleasesubscribe"
                    };
                    
                    for (const auto& standalone : standalone_hallucinations) {
                        if (trimmed == standalone) {
                            if (!quiet_mode) {
                                std::cout << "[filtered standalone hallucination: " << transcribed_text << "] " << std::flush;
                            }
                            is_hallucination = true;
                            break;
                        }
                    }
                }
                
                if (!is_hallucination) {
                    if (!transcription_started) {
                        std::cout << "\nTranscription: ";
                        transcription_started = true;
                    }
                    current_transcription += transcribed_text + " ";
                    // Immediate echo is already done inside transcribeWithWhisper
                }
            }
            
            // Remove processed chunk with smaller overlap to avoid retranscribing same content
            // Use smaller overlap for second and subsequent chunks to avoid confusion
            chunk_count++;
            
            int overlap;
            if (chunk_count == 1) {
                // First chunk: use minimal overlap
                overlap = TRANSCRIPTION_CHUNK_SIZE / 16; // 6.25% overlap (0.1875 seconds)
            } else {
                // Subsequent chunks: use almost no overlap to prevent hallucinations
                overlap = TRANSCRIPTION_CHUNK_SIZE / 32; // 3.125% overlap (0.09375 seconds)
            }
            
            if (!quiet_mode) {
                std::cout << "[chunk " << chunk_count << ", removing " << (TRANSCRIPTION_CHUNK_SIZE - overlap) << " samples, keeping " << overlap << " overlap] " << std::flush;
            }
            audio_buffer.erase(audio_buffer.begin(),
                             audio_buffer.begin() + TRANSCRIPTION_CHUNK_SIZE - overlap);
        }
    }
    
    void finalizeTranscription() {
        if (!audio_buffer.empty() && transcription_started && audio_buffer.size() >= 8000) {
            // For final transcription, only process truly new audio (no overlap with previous chunks)
            // Skip this final processing if we have very little new audio to avoid bad retranscription
            if (audio_buffer.size() >= TRANSCRIPTION_CHUNK_SIZE / 2) { // Only if we have at least 1.5 seconds of new audio
                std::cout << "🔄 " << std::flush;
                std::string final_text = transcribeWithWhisper(audio_buffer);
                if (!final_text.empty()) {
                    current_transcription += final_text;
                    std::cout << final_text << std::flush;
                }
            } else {
                if (!quiet_mode) {
                    std::cout << "[skipping final chunk - too small] " << std::flush;
                }
            }
        }
        
        if (transcription_started) {
            // Clean up transcription
            std::string clean_text = current_transcription;
            
            // Remove extra spaces
            size_t pos = 0;
            while ((pos = clean_text.find("  ", pos)) != std::string::npos) {
                clean_text.replace(pos, 2, " ");
            }
            
            // Trim
            clean_text.erase(0, clean_text.find_first_not_of(" "));
            clean_text.erase(clean_text.find_last_not_of(" ") + 1);
            
            std::cout << "\n\nComplete transcription:\n\"" << clean_text << "\"" << std::endl;
            
            // Calculate stats
            float duration = (float)recorded_samples / 16000.0f;
            std::cout << "Audio: " << duration << "s, Words: " << 
                std::count(clean_text.begin(), clean_text.end(), ' ') + 1 << std::endl;
        }
        
        // Reset for next session
        audio_buffer.clear();
        current_transcription.clear();
        transcription_started = false;
        recorded_samples = 0;
    }
    
    int recorded_samples = 0;
    
    // Helper function to reset the chunk counter for new sessions
    void resetChunkCounter() {
        chunk_count = 0;
    }
    
    void startStreaming() {
        std::cout << "\n=== Real-time Whisper Transcriber Started ===" << std::endl;
        std::cout << "Say '" << hotword << "' to start real-time transcription..." << std::endl;
        std::cout << "Audio will be transcribed using Whisper as you speak." << std::endl;
        std::cout << "Stop speaking for ~2 seconds to end transcription." << std::endl;
        std::cout << "Press Ctrl+C to exit.\n" << std::endl;
        
        std::vector<short> samples;
        int loop_count = 0;
        
        while (true) {
            audio_in->read(samples);
            
            if (!is_listening) {
                // Wait for hotword detection
                auto detection_result = detector->RunDetection(samples.data(), samples.size(), false);
                
                // Show listening indicator every 100 loops (~6 seconds)
                if (++loop_count % 100 == 0) {
                    std::cout << "." << std::flush;
                }
                
                if (detection_result > 0) {
                    std::cout << "\nHOTWORD DETECTED! Starting real-time transcription..." << std::endl;
                    is_listening = true;
                    audio_buffer.clear();
                    silence_counter = 0;
                    speech_counter = 0;
                    current_transcription.clear();
                    transcription_started = false;
                    recorded_samples = 0;
                    loop_count = 0;
                    // Reset chunk counter for new session
                    resetChunkCounter();
                }
            } else {
                // We're listening - analyze speech and transcribe in real-time
                audio_buffer.insert(audio_buffer.end(), samples.begin(), samples.end());
                recorded_samples += samples.size();
                
                // Use VAD to detect speech vs silence
                auto vad_result = vad->RunVad(samples.data(), samples.size());
                
                if (vad_result == -2) {
                    // Silence detected
                    silence_counter++;
                    if (silence_counter % 20 == 0) {
                        std::cout << "." << std::flush; // Show silence progress
                    }
                } else {
                    // Speech detected
                    silence_counter = 0;
                    speech_counter++;
                    if (speech_counter % 10 == 0) {
                        std::cout << "*" << std::flush; // Show speech activity
                    }
                    
                    // Process audio chunk for transcription - be more responsive to fast speech
                    // Reduced requirement: process after fewer speech detections
                    if (speech_counter > MIN_SPEECH_LENGTH / 2048) { // Half the previous requirement
                        processAudioChunk();
                    }
                }
                
                // Also process chunks based on time/buffer size, not just VAD speech counter
                // This ensures fast speech gets processed even if VAD is inconsistent
                if (audio_buffer.size() >= TRANSCRIPTION_CHUNK_SIZE) {
                    if (!quiet_mode) {
                        std::cout << "[buffer full, processing...] " << std::flush;
                    }
                    processAudioChunk();
                }
                
                // Stop listening if we have enough silence
                if (silence_counter >= SILENCE_THRESHOLD) {
                    std::cout << "\nSilence detected. Finalizing transcription..." << std::endl;
                    finalizeTranscription();
                    
                    // Reset for next detection
                    is_listening = false;
                    std::cout << "\nReady for next command. Say '" << hotword << "' to start transcription..." << std::endl;
                }
                
                // Safety: Don't listen indefinitely (max 60 seconds)
                if (audio_buffer.size() > 16000 * 60) {
                    std::cout << "\nWARNING: Maximum listening time reached (60s). Stopping..." << std::endl;
                    finalizeTranscription();
                    is_listening = false;
                }
            }
        }
    }
};

int main(int argc, const char** argv) try {
#ifdef _WIN32
    // Ensure console uses UTF-8 to better display any non-ASCII characters
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
#endif
    std::cout << "Real-time Whisper.cpp Transcriber" << std::endl;
    std::cout << "====================================" << std::endl;
    
    std::string model_path;
    std::string lang = "auto";
    int ngl = 0; // default CPU only; set to e.g. 35 for large-v3 GPU offload
    bool quiet = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind("--lang=", 0) == 0) {
            lang = arg.substr(7);
        } else if (arg == "--gpu") {
            ngl = 35; // reasonable default for large-v3 (32+ few text layers)
        } else if (arg.rfind("--ngl=", 0) == 0) {
            try { ngl = std::stoi(arg.substr(6)); } catch (...) {}
        } else if (arg.rfind("--model=", 0) == 0) {
            model_path = arg.substr(8);
        } else if (arg == "--quiet" || arg == "-q") {
            quiet = true;
        } else if (model_path.empty()) {
            model_path = arg; // backward-compat: first positional is model path
        }
    }
    
    WhisperStreamingTranscriber transcriber(model_path, lang, ngl, quiet);
    transcriber.startStreaming();
    
    return 0;
} catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return -1;
}
