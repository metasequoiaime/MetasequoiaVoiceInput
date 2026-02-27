//
// MetasequoiaVoiceInput Utils
//
#pragma once

#include <string>
#include <Windows.h>

// Convert UTF-8 std::string to std::wstring
namespace mvi_utils
{
std::wstring utf8_to_wstring(const std::string &str);
std::string retrive_token();
std::wstring get_vad_model_path();
std::string get_ggml_model_path();
int GetTaskbarHeight();
RECT GetMonitorCoordinates();
RECT GetMainMonitorCoordinates();
std::wstring resolve_asset_audio_path(std::string filename);
FLOAT GetForegroundWindowScale();
} // namespace mvi_utils
