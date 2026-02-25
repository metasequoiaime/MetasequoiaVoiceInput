# Metasequoia Voice Input

This is a voice input module for [MetasequoiaIME](https://github.com/fanlumaster/MetasequoiaIME). However, it can be used as a standalone voice input tool for any application without other MetasequoiaIME components.

## How to run

Download release exe file from [releases](https://github.com/fanlumaster/MetasequoiaVoiceInput/releases).

Then, copy all the contents of this project's `assets` folder to `$env:LOCALAPPDATA\MetasequoiaVoiceInput\`. And replace your siliconflow token in `config.toml`.

Then, run `MetasequoiaVoiceInput.exe`.

## Usage

- **Hotkeys**:
  - RAlt pressed to start recording, release to stop recording and send text to active application
  - RAlt + Space: Lock recording
  - Ctrl + F9: Toggle recording

## Configuration

Edit `$env:LOCALAPPDATA\MetasequoiaVoiceInput\config.toml` (create if not exists) to configure the application.

Below is a template:

```toml
# 自动语音识别（ASR）配置
[asr_api]
# 服务提供商（如：siliconflow、azure、openai、deepgram 等）
provider = "siliconflow"
# API 访问令牌
token = "<YOUR_OWN_TOKEN>"
# API 基础地址
endpoint = "https://api.siliconflow.cn/v1/audio/transcriptions"

# 文本润色配置
[polish_api]
provider = "siliconflow"
token = "<YOUR_OWN_TOKEN>"
# API 基础地址
endpoint = "https://api.siliconflow.cn/v1/chat/completions"

# 基础设置
[settings]
# 偏好语言
language = "zh-cn"
# 上屏前是否要先进行文本润色
polish_text = true
```

## License

GPL-3.0.
