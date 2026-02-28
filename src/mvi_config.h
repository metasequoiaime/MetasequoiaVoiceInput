#pragma once

#include <string>

namespace mvi_config
{
std::string GetConfigPath();
std::string GetApiToken(std::string api_type = "asr");
std::string GetLanguage();
bool GetPolishTextEnabled();
std::string GetSTTProvider();
} // namespace mvi_config
