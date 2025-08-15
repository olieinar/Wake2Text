/**
 * Wake2Text - Real-time hotword-activated speech-to-text transcription
 *
 * Combines snowman hotword detection with OpenAI's Whisper C API for accurate
 * real-time transcription. Supports Windows (WinMM) and Linux (PulseAudio).
 */

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
#include <vector>
#include "snowboy-detect.h"
#include "pulseaudio.hh"
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4244) // Suppress conversion warnings from Whisper.cpp
#endif
#include "whisper.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#ifdef _WIN32
#include <windows.h>
#include <filesystem>
#else
#include <sys/wait.h>
#endif

namespace pa = pulseaudio::pa;

class WhisperStreamingTranscriber
{
private:
    std::string root;
    std::string model;
    std::string hotword;
    pa::simple_record_stream *audio_in;
    snowboy::SnowboyDetect *detector;
    snowboy::SnowboyVad *vad;

    // Whisper C API integration
    struct whisper_context *whisper_ctx;
    struct whisper_full_params whisper_params;
    std::string whisper_model_path;
    std::string lang_code = "en";
    int ngl_layers = 0;

    std::vector<short> audio_buffer;
    bool is_listening = false;
    int silence_counter = 0;
    int speech_counter = 0;
    const int SILENCE_THRESHOLD = 30;
    const int MIN_SPEECH_LENGTH = 8000;
    const int TRANSCRIPTION_CHUNK_SIZE = 48000;

    std::string current_transcription;
    bool transcription_started = false;
    int chunk_count = 0;
    bool quiet_mode = false;

    // Convert short samples to float samples (Whisper expects float)
    std::vector<float> convertToFloat(const std::vector<short> &audio_data)
    {
        std::vector<float> float_data;
        float_data.reserve(audio_data.size());
        for (short sample : audio_data)
        {
            float_data.push_back(static_cast<float>(sample) / 32768.0f);
        }
        return float_data;
    }

    // Check for common Whisper hallucinations
    bool isHallucination(const std::string &text)
    {
        std::string lower_text = text;
        std::transform(lower_text.begin(), lower_text.end(), lower_text.begin(), ::tolower);

        std::vector<std::string> hallucinations = {
            "υπότιτλοι", "authorwave", "subtitles", "subtitle", "closed captions",
            "captioning", "transcription", "transcript", "audio", "music",
            "[music]", "[sound]", "[noise]", "[silence]", "[inaudible]",
            "thank you", "thanks for watching", "subscribe", "like and subscribe",
            "www.", ".com", "http", "https",
            "undertekster", "ai-media", "ai media", "undertekst", "tekster",
            "untertitel", "sous-titres", "legendas", "sottotitoli"};

        for (const auto &halluc : hallucinations)
        {
            if (lower_text.find(halluc) != std::string::npos)
            {
                return true;
            }
        }

        // Check for standalone hallucinations
        std::string trimmed = lower_text;
        trimmed.erase(std::remove_if(trimmed.begin(), trimmed.end(),
                                     [](char c)
                                     { return c == '.' || c == ',' || c == '!' || c == '?' || c == ' ' || c == '\t' || c == '\n' || c == '\r'; }),
                      trimmed.end());

        std::vector<std::string> standalone_hallucinations = {
            "thankyou", "thankyouforwatching", "thanks", "thanksforwatching",
            "subscribe", "likeandsubscribe", "pleasesubscribe"};

        for (const auto &standalone : standalone_hallucinations)
        {
            if (trimmed == standalone)
            {
                return true;
            }
        }

        return false;
    }

public:
    WhisperStreamingTranscriber(const std::string &model_path = "", const std::string &language = "en", int ngl = 0, bool quiet = false)
    {
        ngl_layers = ngl;
        lang_code = language;
        quiet_mode = quiet;
        whisper_ctx = nullptr;

#ifdef _WIN32
        char module_path[MAX_PATH];
        GetModuleFileNameA(NULL, module_path, MAX_PATH);
        std::filesystem::path exe_dir = std::filesystem::path(module_path).parent_path();
#else
        std::filesystem::path exe_dir = std::filesystem::current_path();
#endif
        root = "";

        std::filesystem::path base = std::filesystem::path(detect_project_root());
        std::filesystem::path default_model = base / "resources" / "pmdl" / "hey_casper.pmdl";
        std::filesystem::path large_v3_project = base / "models" / "ggml-large-v3.bin";
        std::filesystem::path large_v3_whisper = base / "whisper.cpp" / "models" / "ggml-large-v3.bin";

        model = model_path.empty() ? default_model.string() : model_path;

        // Find Whisper model
        if (std::filesystem::exists(large_v3_project))
        {
            whisper_model_path = std::filesystem::absolute(large_v3_project).string();
        }
        else if (std::filesystem::exists(large_v3_whisper))
        {
            whisper_model_path = std::filesystem::absolute(large_v3_whisper).string();
        }
        else
        {
            std::string msg = "Required model ggml-large-v3.bin not found. Checked:\n  " + (large_v3_project.string()) + "\n  " + (large_v3_whisper.string()) +
                              "\nYou can download it with: whisper.cpp\\models\\download-ggml-model.cmd large-v3";
            throw std::runtime_error(msg);
        }

#ifdef _WIN32
        auto to_win_path = [](std::string s)
        {
            for (auto &c : s)
                if (c == '/')
                    c = '\\';
            return s;
        };
        whisper_model_path = to_win_path(whisper_model_path);
        model = to_win_path(model);
#endif

        // Initialize Whisper
        struct whisper_context_params cparams = whisper_context_default_params();
        cparams.use_gpu = (ngl_layers > 0);

        whisper_ctx = whisper_init_from_file_with_params(whisper_model_path.c_str(), cparams);
        if (!whisper_ctx)
        {
            throw std::runtime_error("Failed to initialize Whisper model: " + whisper_model_path);
        }

        // Setup Whisper parameters
        whisper_params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        whisper_params.language = lang_code.c_str();
        whisper_params.n_threads = static_cast<int>(std::thread::hardware_concurrency() / 2);
        whisper_params.offset_ms = 0;
        whisper_params.duration_ms = 0;
        whisper_params.translate = false;
        whisper_params.no_context = true; // Disable context to prevent overlap issues
        whisper_params.single_segment = false;
        whisper_params.print_special = false;
        whisper_params.print_progress = false;
        whisper_params.print_realtime = false;
        whisper_params.print_timestamps = false;

        // Quality settings equivalent to --best-of 5 --beam-size 5
        whisper_params.strategy = WHISPER_SAMPLING_BEAM_SEARCH;
        whisper_params.beam_search.beam_size = 5;
        whisper_params.greedy.best_of = 5;

        // Quality thresholds
        whisper_params.no_speech_thold = 0.6f; // Higher threshold to reduce false positives
        whisper_params.temperature = 0.0f;
        whisper_params.suppress_blank = true;
        whisper_params.suppress_nst = true;

        // Determine hotword name from model file
        hotword = "unknown";
        if (model.find("computer.umdl") != std::string::npos)
            hotword = "computer";
        else if (model.find("jarvis.umdl") != std::string::npos)
            hotword = "jarvis";
        else if (model.find("hey_extreme.umdl") != std::string::npos)
            hotword = "hey extreme";
        else if (model.find("alexa.umdl") != std::string::npos)
            hotword = "alexa";
        else if (model.find("hey_casper.pmdl") != std::string::npos)
            hotword = "hey casper";
        else if (model.find(".pmdl") != std::string::npos)
        {
            size_t lastSlash = model.find_last_of("/\\");
            size_t lastDot = model.find_last_of(".");
            if (lastSlash != std::string::npos && lastDot != std::string::npos)
            {
                hotword = model.substr(lastSlash + 1, lastDot - lastSlash - 1);
                std::replace(hotword.begin(), hotword.end(), '_', ' ');
            }
        }

        // Initialize audio and detection
        audio_in = new pa::simple_record_stream("Whisper Streaming Transcriber");
        detector = new snowboy::SnowboyDetect(root + "resources/common.res", model);
        vad = new snowboy::SnowboyVad(root + "resources/common.res");

        detector->SetSensitivity("0.45");
        detector->SetAudioGain(1.5);
        detector->ApplyFrontend(true);

        if (!quiet_mode)
        {
            std::cout << "[init] Whisper Streaming Transcriber initialized (C API)" << std::endl;
            std::cout << "Hotword: '" << hotword << "'" << std::endl;
            std::cout << "Model: " << model << std::endl;
            std::cout << "Whisper model: " << whisper_model_path << std::endl;
            std::cout << "Language: " << lang_code << std::endl;
            std::cout << "GPU offload: " << (cparams.use_gpu ? "enabled" : "disabled") << std::endl;
        }
    }

    ~WhisperStreamingTranscriber()
    {
        if (whisper_ctx)
        {
            whisper_free(whisper_ctx);
        }
        delete audio_in;
        delete detector;
        delete vad;
    }

    std::string transcribeWithWhisper(const std::vector<short> &audio_chunk)
    {
        if (!whisper_ctx)
        {
            return "[Whisper not initialized]";
        }

        if (!quiet_mode)
        {
            std::cout << "[proc] " << std::flush;
        }

        // Convert audio to float format
        std::vector<float> float_audio = convertToFloat(audio_chunk);

        // Run Whisper transcription
        if (whisper_full(whisper_ctx, whisper_params, float_audio.data(), static_cast<int>(float_audio.size())) != 0)
        {
            if (!quiet_mode)
            {
                std::cout << "[ERROR] Whisper transcription failed" << std::endl;
            }
            return "";
        }

        // Extract transcribed text
        std::string result;
        const int n_segments = whisper_full_n_segments(whisper_ctx);
        for (int i = 0; i < n_segments; ++i)
        {
            const char *text = whisper_full_get_segment_text(whisper_ctx, i);
            if (text && text[0] != '\0')
            {
                std::string segment_text = text;

                // Trim whitespace
                size_t start = segment_text.find_first_not_of(" \t\n\r");
                size_t end = segment_text.find_last_not_of(" \t\n\r");
                if (start != std::string::npos)
                {
                    segment_text = segment_text.substr(start, end - start + 1);

                    if (!isHallucination(segment_text))
                    {
                        if (!result.empty())
                            result += " ";
                        result += segment_text;
                        std::cout << segment_text << " " << std::flush;
                    }
                    else
                    {
                        if (!quiet_mode)
                        {
                            std::cout << "[filtered: " << segment_text << "] " << std::flush;
                        }
                    }
                }
            }
        }

        return result;
    }

    bool hasSubstantialSpeech(const std::vector<short> &audio_chunk)
    {
        if (audio_chunk.empty())
            return false;

        long long sum_squares = 0;
        for (short sample : audio_chunk)
        {
            sum_squares += (long long)sample * sample;
        }
        double rms = sqrt((double)sum_squares / audio_chunk.size());

        const double MIN_RMS_THRESHOLD = 50.0;

        if (rms < MIN_RMS_THRESHOLD)
        {
            if (!quiet_mode)
            {
                std::cout << "[near silence: RMS=" << (int)rms << "] " << std::flush;
            }
            return false;
        }

        int speech_samples = 0;
        const short SPEECH_THRESHOLD = 200;
        for (short sample : audio_chunk)
        {
            if (abs(sample) > SPEECH_THRESHOLD)
            {
                speech_samples++;
            }
        }

        double speech_ratio = (double)speech_samples / audio_chunk.size();
        if (speech_ratio < 0.005)
        {
            if (!quiet_mode)
            {
                std::cout << "[no audio activity: " << (int)(speech_ratio * 1000) << "‰] " << std::flush;
            }
            return false;
        }

        if (!quiet_mode)
        {
            std::cout << "[audio OK: RMS=" << (int)rms << ", activity=" << (int)(speech_ratio * 100) << "%] " << std::flush;
        }
        return true;
    }

    void processAudioChunk()
    {
        if (audio_buffer.size() >= TRANSCRIPTION_CHUNK_SIZE)
        {
            std::vector<short> chunk(audio_buffer.begin(),
                                     audio_buffer.begin() + TRANSCRIPTION_CHUNK_SIZE);

            if (!hasSubstantialSpeech(chunk))
            {
                if (!quiet_mode)
                {
                    std::cout << "[skipping chunk - insufficient speech] " << std::flush;
                }
                int overlap = TRANSCRIPTION_CHUNK_SIZE / 8; // Smaller overlap for skipped chunks
                audio_buffer.erase(audio_buffer.begin(),
                                   audio_buffer.begin() + TRANSCRIPTION_CHUNK_SIZE - overlap);
                return;
            }

            std::string transcribed_text = transcribeWithWhisper(chunk);

            if (!transcribed_text.empty())
            {
                if (!transcription_started)
                {
                    std::cout << "\nTranscription: ";
                    transcription_started = true;
                }
                current_transcription += transcribed_text + " ";
            }

            chunk_count++;

            int overlap;
            if (chunk_count == 1)
            {
                overlap = TRANSCRIPTION_CHUNK_SIZE / 32; // Very small overlap for first chunk
            }
            else
            {
                overlap = TRANSCRIPTION_CHUNK_SIZE / 64; // Minimal overlap for subsequent chunks
            }

            if (!quiet_mode)
            {
                std::cout << "[chunk " << chunk_count << ", removing " << (TRANSCRIPTION_CHUNK_SIZE - overlap) << " samples, keeping " << overlap << " overlap] " << std::flush;
            }
            audio_buffer.erase(audio_buffer.begin(),
                               audio_buffer.begin() + TRANSCRIPTION_CHUNK_SIZE - overlap);
        }
    }

    void finalizeTranscription()
    {
        if (!audio_buffer.empty() && transcription_started && audio_buffer.size() >= 8000)
        {
            if (audio_buffer.size() >= TRANSCRIPTION_CHUNK_SIZE / 2)
            {
                std::cout << "🔄 " << std::flush;
                std::string final_text = transcribeWithWhisper(audio_buffer);
                if (!final_text.empty())
                {
                    current_transcription += final_text;
                    std::cout << final_text << std::flush;
                }
            }
            else
            {
                if (!quiet_mode)
                {
                    std::cout << "[skipping final chunk - too small] " << std::flush;
                }
            }
        }

        if (transcription_started)
        {
            std::string clean_text = current_transcription;

            size_t pos = 0;
            while ((pos = clean_text.find("  ", pos)) != std::string::npos)
            {
                clean_text.replace(pos, 2, " ");
            }

            clean_text.erase(0, clean_text.find_first_not_of(" "));
            clean_text.erase(clean_text.find_last_not_of(" ") + 1);

            std::cout << "\n\nComplete transcription:\n\"" << clean_text << "\"" << std::endl;

            float duration = (float)recorded_samples / 16000.0f;
            std::cout << "Audio: " << duration << "s, Words: " << std::count(clean_text.begin(), clean_text.end(), ' ') + 1 << std::endl;
        }

        audio_buffer.clear();
        current_transcription.clear();
        transcription_started = false;
        recorded_samples = 0;
    }

    int recorded_samples = 0;

    void resetChunkCounter()
    {
        chunk_count = 0;
    }

    void startStreaming()
    {
        std::cout << "\n=== Real-time Whisper Transcriber Started (C API) ===" << std::endl;
        std::cout << "Say '" << hotword << "' to start real-time transcription..." << std::endl;
        std::cout << "Audio will be transcribed using Whisper as you speak." << std::endl;
        std::cout << "Stop speaking for ~2 seconds to end transcription." << std::endl;
        std::cout << "Press Ctrl+C to exit.\n"
                  << std::endl;

        std::vector<short> samples;
        int loop_count = 0;

        while (true)
        {
            audio_in->read(samples);

            if (!is_listening)
            {
                auto detection_result = detector->RunDetection(samples.data(), samples.size(), false);

                if (++loop_count % 100 == 0)
                {
                    std::cout << "." << std::flush;
                }

                if (detection_result > 0)
                {
                    std::cout << "\nHOTWORD DETECTED! Starting real-time transcription..." << std::endl;
                    is_listening = true;
                    audio_buffer.clear();
                    silence_counter = 0;
                    speech_counter = 0;
                    current_transcription.clear();
                    transcription_started = false;
                    recorded_samples = 0;
                    loop_count = 0;
                    resetChunkCounter();
                }
            }
            else
            {
                audio_buffer.insert(audio_buffer.end(), samples.begin(), samples.end());
                recorded_samples += samples.size();

                auto vad_result = vad->RunVad(samples.data(), samples.size());

                if (vad_result == -2)
                {
                    silence_counter++;
                    if (silence_counter % 20 == 0)
                    {
                        std::cout << "." << std::flush;
                    }
                }
                else
                {
                    silence_counter = 0;
                    speech_counter++;
                    if (speech_counter % 10 == 0)
                    {
                        std::cout << "*" << std::flush;
                    }

                    if (speech_counter > MIN_SPEECH_LENGTH / 2048)
                    {
                        processAudioChunk();
                    }
                }

                if (audio_buffer.size() >= TRANSCRIPTION_CHUNK_SIZE)
                {
                    if (!quiet_mode)
                    {
                        std::cout << "[buffer full, processing...] " << std::flush;
                    }
                    processAudioChunk();
                }

                if (silence_counter >= SILENCE_THRESHOLD)
                {
                    std::cout << "\nSilence detected. Finalizing transcription..." << std::endl;
                    finalizeTranscription();

                    is_listening = false;
                    std::cout << "\nReady for next command. Say '" << hotword << "' to start transcription..." << std::endl;
                }

                if (audio_buffer.size() > 16000 * 60)
                {
                    std::cout << "\nWARNING: Maximum listening time reached (60s). Stopping..." << std::endl;
                    finalizeTranscription();
                    is_listening = false;
                }
            }
        }
    }
};

void print_usage()
{
    std::cout << "Usage: wake2text [options]" << std::endl;
    std::cout << "\nOptions:" << std::endl;
    std::cout << "  --help              Show this help message and exit" << std::endl;
    std::cout << "  --model=<path>      Path to hotword model file (.pmdl or .umdl)" << std::endl;
    std::cout << "  --lang=<code>       Language code (default: auto)" << std::endl;
    std::cout << "                      Examples: en, fr, de, es, zh, ja, ko, etc." << std::endl;
    std::cout << "  --gpu               Enable GPU acceleration (requires CUDA)" << std::endl;
    std::cout << "  --ngl=<n>           Number of GPU layers to offload (default: 0 = CPU only)" << std::endl;
    std::cout << "  --quiet, -q         Quiet mode (minimal output)" << std::endl;
    std::cout << "\nExamples:" << std::endl;
    std::cout << "  wake2text                          Use default hotword model with auto language detection" << std::endl;
    std::cout << "  wake2text --model=custom.pmdl      Use custom hotword model" << std::endl;
    std::cout << "  wake2text --lang=en --gpu          Use English language with GPU acceleration" << std::endl;
}

int main(int argc, const char **argv)
try
{
#ifdef _WIN32
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
#endif
    std::cout << "Real-time Whisper.cpp Transcriber (C API)" << std::endl;
    std::cout << "=========================================" << std::endl;

    std::string model_path;
    std::string lang = "auto";
    int ngl = 0;
    bool quiet = false;
    bool show_help = false;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h")
        {
            show_help = true;
        }
        else if (arg.rfind("--lang=", 0) == 0)
        {
            lang = arg.substr(7);
        }
        else if (arg == "--gpu")
        {
            ngl = 35;
        }
        else if (arg.rfind("--ngl=", 0) == 0)
        {
            try
            {
                ngl = std::stoi(arg.substr(6));
            }
            catch (...)
            {
            }
        }
        else if (arg.rfind("--model=", 0) == 0)
        {
            model_path = arg.substr(8);
        }
        else if (arg == "--quiet" || arg == "-q")
        {
            quiet = true;
        }
        else if (model_path.empty())
        {
            model_path = arg;
        }
    }

    if (show_help)
    {
        print_usage();
        return 0;
    }

    WhisperStreamingTranscriber transcriber(model_path, lang, ngl, quiet);
    transcriber.startStreaming();

    return 0;
}
catch (const std::exception &e)
{
    std::cerr << "Error: " << e.what() << std::endl;
    return -1;
}