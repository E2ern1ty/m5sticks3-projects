# Pocket Translator · 口袋翻译器

按住 A 说中文，松开即得英文/日文译文并朗读。

```
按住 A ─► 讯飞 iat 流式听写（边说边出字，wss 推流）
松开   ─► 硅基流动 Qwen2.5-7B 翻译(~0.4s) ─► CosyVoice2 合成朗读(~0.8s)
```

- B 键切换 英↔日（NVS 记忆）；重力横屏翻转；TTS 语速 0.8、音量满格
- iat 失败自动回退整段 HTTPS 识别（Qwen3-ASR）；TTS 失败降级纯文字
- 错误页 A=回放录音、B=继续

## 配置（必做）

```bash
cp secrets.example.h secrets.h
# 填：WiFi、硅基流动 key、讯飞三件套（iat 语音听写）
```

- 硅基流动 https://siliconflow.cn ：ASR `Qwen/Qwen3-ASR-1.7B`、
  翻译 `Qwen/Qwen2.5-7B-Instruct`、TTS `FunAudioLLM/CosyVoice2-0.5B`
- 讯飞 https://www.xfyun.cn ：开通"语音听写（流式版）"，
  三件套注意 hex 串=APIKey、base64 串=APISecret

## 烧录

免编译包含占位密钥，**烧后需改 secrets.h 重编**才能联网使用——
所以翻译器实际上必须源码构建（见根 README）。
接口排障脚本：`test_api.py`（硅基流动三步）、`test_xfyun.py`（讯飞 iat/ist 探测记录）。
