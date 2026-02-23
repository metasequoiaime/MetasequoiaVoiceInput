#include "mvi_config.h"
#include <cstdlib>
#include <exception>
#include <toml++/toml.hpp>

namespace
{
std::string get_localappdata_dir()
{
    char *buf = nullptr;
    size_t sz = 0;
    std::string localappdata;
    if (_dupenv_s(&buf, &sz, "LOCALAPPDATA") == 0 && buf != nullptr)
    {
        localappdata = buf;
        free(buf);
    }
    return localappdata;
}
} // namespace

std::string mvi_config::GetConfigPath()
{
    const std::string localappdata = get_localappdata_dir();
    if (localappdata.empty())
    {
        return "";
    }
    return localappdata + "\\MetasequoiaVoiceInput\\config.toml";
}

std::string mvi_config::GetApiToken(std::string api_type)
{
    const std::string config_path = GetConfigPath();
    if (config_path.empty())
    {
        return "";
    }

    try
    {
        const toml::table config = toml::parse_file(config_path);
        std::string node_api_type;
        if (api_type == "asr")
        {
            node_api_type = "asr_api";
        }
        else
        {
            node_api_type = "polish_api";
        }
        if (const toml::node_view<const toml::node> token_node = config[node_api_type]["token"]; token_node.is_string())
        {
            return token_node.value_or("");
        }
    }
    catch (const toml::parse_error &)
    {
        return "";
    }
    catch (const std::exception &)
    {
        return "";
    }

    return "";
}

std::string mvi_config::GetLanguage()
{
    const std::string config_path = GetConfigPath();
    if (config_path.empty())
    {
        return "zh-cn";
    }

    try
    {
        const toml::table config = toml::parse_file(config_path);
        if (const toml::node_view<const toml::node> language_node = config["settings"]["language"]; language_node.is_string())
        {
            return language_node.value_or("zh-cn");
        }
    }
    catch (const toml::parse_error &)
    {
        return "zh-cn";
    }
    catch (const std::exception &)
    {
        return "zh-cn";
    }

    return "zh-cn";
}

bool mvi_config::GetPolishTextEnabled()
{
    const std::string config_path = GetConfigPath();
    if (config_path.empty())
    {
        return false;
    }

    try
    {
        const toml::table config = toml::parse_file(config_path);
        if (const toml::node_view<const toml::node> polish_node = config["settings"]["polish_text"]; polish_node.is_boolean())
        {
            return polish_node.value_or(false);
        }
    }
    catch (const toml::parse_error &)
    {
        return false;
    }
    catch (const std::exception &)
    {
        return false;
    }

    return false;
}
