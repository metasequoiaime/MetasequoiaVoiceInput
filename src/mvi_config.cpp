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

std::string mvi_config::GetApiToken()
{
    const std::string config_path = GetConfigPath();
    if (config_path.empty())
    {
        return "";
    }

    try
    {
        const toml::table config = toml::parse_file(config_path);
        if (const toml::node_view<const toml::node> token_node = config["api"]["token"]; token_node.is_string())
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
