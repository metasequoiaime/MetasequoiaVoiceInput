#include <iostream>
#include <string>
#include <cstdio>
#include <memory>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <atomic>
#include <stdexcept>
#include "silero_vad.h"
#include "audio_capture.h"
#include "whisper_worker.h"
#include "cloud_stt_worker.h"
#include "send_input.h"
#include "mvi_utils.h"
#include <fmt/format.h>
#include <windows.h>

// Configuration
enum class SttProvider
{
    LocalWhisper,
    CloudSiliconFlow
};

SttProvider g_stt_provider = SttProvider::CloudSiliconFlow; // Default to Cloud for testing
std::string g_cloud_token;

namespace
{
constexpr UINT WM_APP_RALT_TOGGLE = WM_APP + 1;
constexpr UINT WM_APP_EXIT = WM_APP + 2;

std::atomic<bool> g_lctrl_pressed{false};
std::atomic<bool> g_rctrl_pressed{false};
std::atomic<bool> g_f9_pressed{false};
DWORD g_main_thread_id = 0;

bool is_ctrl_pressed()
{
    return g_lctrl_pressed.load() || g_rctrl_pressed.load();
}

LRESULT CALLBACK keyboard_hook_proc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION)
    {
        const auto *kb = reinterpret_cast<KBDLLHOOKSTRUCT *>(lParam);
        if (kb != nullptr && (kb->vkCode == VK_LCONTROL || kb->vkCode == VK_RCONTROL))
        {
            const bool is_key_down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
            const bool is_key_up = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);

            if (is_key_down)
            {
                if (kb->vkCode == VK_LCONTROL)
                {
                    bool expected = false;
                    g_lctrl_pressed.compare_exchange_strong(expected, true);
                }
                else
                {
                    bool expected = false;
                    g_rctrl_pressed.compare_exchange_strong(expected, true);
                }
            }
            else if (is_key_up)
            {
                if (kb->vkCode == VK_LCONTROL)
                {
                    bool expected = true;
                    g_lctrl_pressed.compare_exchange_strong(expected, false);
                }
                else
                {
                    bool expected = true;
                    g_rctrl_pressed.compare_exchange_strong(expected, false);
                }
            }
        }
        else if (kb != nullptr && kb->vkCode == VK_F9)
        {
            const bool is_key_down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
            const bool is_key_up = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);

            if (is_key_down)
            {
                bool expected = false;
                if (g_f9_pressed.compare_exchange_strong(expected, true))
                {
                    if (is_ctrl_pressed())
                    {
                        PostThreadMessage(g_main_thread_id, WM_APP_RALT_TOGGLE, 0, 0);
                        return 1; // 吞下组合键中的 F9
                    }
                }
            }
            else if (is_key_up)
            {
                bool expected = true;
                g_f9_pressed.compare_exchange_strong(expected, false);
                if (is_ctrl_pressed())
                {
                    return 1; // Ctrl 按住时吞掉 F9 抬起
                }
            }
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}
} // namespace

int main()
{
    // g_stt_provider = SttProvider::LocalWhisper;

    // Set console code page to UTF-8 so console can display UTF-8 characters correctly
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    // Set up token
    g_cloud_token = mvi_utils::retrive_token();

    // silero_vad.onnx path
    std::wstring vad_model_path = mvi_utils::get_vad_model_path();

    // ggml model path
    std::string ggml_model_path = mvi_utils::get_ggml_model_path();

    printf("--- METASEQUOIA VOICE INPUT START ---\n");
    fflush(stdout);

    try
    {
        printf("[INIT] Loading Silero VAD...\n");
        fflush(stdout);
        SileroVad vad(vad_model_path);
        printf("[INIT] VAD Loaded.\n");
        fflush(stdout);

        std::unique_ptr<SttService> stt;

        if (g_stt_provider == SttProvider::LocalWhisper)
        {
            printf("[INIT] Loading Local Whisper model...\n");
            stt = std::make_unique<WhisperWorker>(ggml_model_path.c_str());
            printf("[INIT] Local Whisper Loaded.\n");
        }
        else if (g_stt_provider == SttProvider::CloudSiliconFlow)
        {
            printf("[INIT] Initializing Cloud STT (SiliconFlow)...\n");
            stt = std::make_unique<CloudSttWorker>(g_cloud_token);
            printf("[INIT] Cloud STT Ready.\n");
        }
        fflush(stdout);

        printf("[INIT] Initializing Audio...\n");
        fflush(stdout);
        AudioCapture audio;
        //
        // STT queue + thread, 把耗时的操作放在这个线程里，避免在 audio callback 里做耗时操作导致丢音频
        //
        std::mutex stt_mutex;
        std::condition_variable stt_cv;
        std::deque<SpeechSegment> stt_queue;
        std::atomic<bool> stt_stop = false;
        std::thread stt_thread([&]() {
            while (!stt_stop)
            {
                SpeechSegment seg;

                {
                    std::unique_lock<std::mutex> lock(stt_mutex);
                    stt_cv.wait(lock, [&]() { return stt_stop || !stt_queue.empty(); });

                    if (stt_stop)
                        break;

                    seg = std::move(stt_queue.front());
                    stt_queue.pop_front();
                }

                // 只有这里才允许慢操作
                auto start = std::chrono::steady_clock::now();
                std::string text = stt->recognize(seg.samples);
                auto end = std::chrono::steady_clock::now();
                std::cout << "[STT] Time: " << std::chrono::duration<double>(end - start).count() << "s\n";
                if (!text.empty())
                {
                    printf("[STT] Recognized: %s\n", text.c_str());
                    fflush(stdout);
                    send_text(mvi_utils::utf8_to_wstring(text));
                }
            }
        });

        auto audio_callback = [&](const float *data, size_t count) {
            try
            {
                vad.push_audio(data, count);
                while (vad.has_segment())
                {
                    auto segment = vad.pop_segment();
                    {
                        std::lock_guard<std::mutex> lock(stt_mutex);
                        stt_queue.push_back(std::move(segment));
                    }
                    stt_cv.notify_one();
                }
            }
            catch (const std::exception &e)
            {
                printf("[CALLBACK ERROR] %s\n", e.what());
                fflush(stdout);
            }
            catch (...)
            {
                printf("[CALLBACK ERROR] Unknown exception\n");
                fflush(stdout);
            }
        };

        g_main_thread_id = GetCurrentThreadId();

        MSG init_msg{};
        PeekMessage(&init_msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

        HHOOK keyboard_hook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboard_hook_proc, GetModuleHandleW(nullptr), 0);
        if (keyboard_hook == nullptr)
        {
            throw std::runtime_error(fmt::format("SetWindowsHookExW failed: {}", GetLastError()));
        }

        std::thread exit_thread([&]() {
            std::cin.get();
            PostThreadMessage(g_main_thread_id, WM_APP_EXIT, 0, 0);
        });

        bool audio_started = false;

        printf("Press Ctrl + F9 to toggle recording. Press ENTER to stop and exit.\n");
        fflush(stdout);

        MSG msg{};
        bool should_exit = false;
        while (!should_exit && GetMessage(&msg, nullptr, 0, 0) > 0)
        {
            switch (msg.message)
            {
            case WM_APP_RALT_TOGGLE: {
                if (!audio_started)
                {
                    if (!audio.start(audio_callback))
                    {
                        printf("[AUDIO] Failed to start capture.\n");
                        fflush(stdout);
                    }
                    else
                    {
                        audio_started = true;
                        printf("[AUDIO] Started.\n");
                        fflush(stdout);
                    }
                }
                else
                {
                    audio.stop();
                    audio_started = false;
                    printf("[AUDIO] Stopped.\n");
                    fflush(stdout);
                }
                break;
            }
            case WM_APP_EXIT:
                should_exit = true;
                break;
            default:
                break;
            }
        }

        printf("Stopping...\n");
        fflush(stdout);

        if (audio_started)
        {
            audio.stop();
        }

        UnhookWindowsHookEx(keyboard_hook);

        if (exit_thread.joinable())
        {
            exit_thread.join();
        }

        // 通知 stt 线程停止
        stt_stop = true;
        stt_cv.notify_one();
        if (stt_thread.joinable())
        {
            stt_thread.join();
        }
        printf("Stopped.\n");
        fflush(stdout);
    }
    catch (const std::exception &e)
    {
        printf("FATAL ERROR: %s\n", e.what());
        fflush(stdout);
        MessageBoxA(nullptr, e.what(), "MetasequoiaVoiceInput - Fatal Error", MB_ICONERROR);
        return 1;
    }

    return 0;
}
