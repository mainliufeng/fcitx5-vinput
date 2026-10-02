# 语音质量测试

这里的工具直接重放生产 ASR 后端。它不占用麦克风、不修改日常配置，不启用 LLM 整理。
实际首次结果与限制见 [2026-10-02 报告](../../docs/quality/2026-10-02/report.md)。

## 已有音频

本机 `corpus/` 中有 **127 个可执行案例**：30 条 FLEURS 真人朗读、60 条受控白噪声变体、
30 条叠加真实火车背景录音的变体、4 条同一 fold 内拼接的长语音、3 条无目标语音案例。
30 条基础录音按语言与原始句子 ID 去重，开发集和保留集不共享原始句子。

加噪和拼接是压力测试，不能视为更多独立说话人，也不能代表真实咖啡馆、办公室或耳机录音。
`recording-prompts.jsonl` 另有 40 条录音脚本，覆盖编程中英混说、否定、数字、纠正、口水词和长段口述；
它们**尚未录音，不计入 127 个已执行案例**。用户自然口述、真实轻声、口音与实际麦克风噪声尚待采集。

音频与临时缓存在 `corpus/` 中，Git 忽略；快照清单、音频 SHA256、实际输出及评分已提交到报告目录。
重新获取需要网络、Python 3、curl、ffmpeg：

```bash
python3 tests/quality/quality.py prepare tests/quality/corpus
python3 tests/quality/quality.py environment tests/quality/corpus
python3 tests/quality/quality.py verify tests/quality/corpus/manifest.jsonl
```

FLEURS 固定数据版本 `70bb2e84b976b7e960aa89f1c648e09c59f894dd`。下载服务的 `test` 分区当时
持续返回 HTTP 500，因此两个 fold 都从 `validation` 按原始句子 ID 分开选取，不能称为官方 test 集。
如果下载签名过期，删除 `corpus/*-validation.json` 后重试；准备过程会复用已完成的 WAV。

## 重放和评分

需要已安装本机的 X-ASR 流式、X-ASR 离线、Silero VAD 模型，以及当前有效的 local provider。
重放工具固定选用上述 X-ASR 模型，继承当前 provider 的热词；`sensevoice` 模式固定选用 SenseVoice 并清空热词。
不得将其他机器/配置的结果直接混入本轮比较。

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --target asr_replay test_vad_padding test_refining_quality test_pipewire_target -j2
ctest --test-dir build --output-on-failure
build/tests/asr_replay tests/quality/corpus/manifest.jsonl refined vad > /tmp/refined.jsonl 2>/tmp/refined.log
python3 tests/quality/quality.py score tests/quality/corpus/manifest.jsonl /tmp/refined.jsonl /tmp/refined-score.json
```

模式为 `streaming`、`refined`、`offline` 或 `sensevoice`；最后参数为 `vad` 或 `no-vad`。
`offline`/`sensevoice` 直接整段重放不包含两遍模式的 30 秒保护，不应对未知超长输入运行。

- CER：NFKC、大小写和空白归一化，去除标点/符号后的字符编辑率。
- 英文使用词错误率；中英混合则逐个汉字与英文单词分词，字段为 `mixed_token_error_rate`。
  必须同时看此指标，因为纯 CER 会隐藏 `hello world` → `helloworld` 的错误。
- `raw_error_rate` 保留标点和符号，但仍归一化空白；它不是标点 F1，也不是语义忠实度。
- 否定、金额、版本、路径和编程术语还需人工检查，不能由低 CER 代替。
- `finish_ms` 是后端结束阶段计算耗时；没有包括录音、应用上屏、网络或设备延迟。
  1.28 秒尾部补齐是快速喂给模型的样本，实际没有睡眠 1.28 秒。
- `init_ms` 分开记录冷启动/会话准备。`first_partial_audio_ms` 是首次文字出现时已喂入的音频长度，
  不是实时墙钟首字延迟。重放以 20 ms 样本块尽快执行。

缺失、额外、重复 ID 或失败结果都会使评分退出，不能把未跑案例算作 0 错误。
不要按参考答案改热词或替换词表来提高保留集成绩。

## 真正的录音链路

在独立 D-Bus 总线和命名虚拟音频设备上，使用正式 daemon 做 PipeWire 实时回放，并检查最终
`RecognitionResult.commit_text`。正常语音必须非空，白噪声和火车噪声必须为空。测试结束卸载虚拟设备。
先确保没有同名 `vinput_quality_probe` 虚拟设备，工具需要本机 PipeWire/PulseAudio 与已安装模型。

```bash
dbus-run-session -- python3 tests/quality/pipewire_check.py tests/quality/corpus /tmp/vinput-live-check
# 验证安装后的 daemon：增加 --daemon /实际路径/vinput-daemon
```

这是录音到最终提交文本的验收；没有验证 Fcitx 插件向真实编辑器插入文字，也没有测 LLM 整理。

## 微信、豆包对标协议

当前两个客户端均不可访问，状态是 **not_measured**，不能推断胜负或达到同等水平。
官方特性用于设计场景：中文/方言、中英混说、专业术语、噪声、轻声，以及口语整理。

获得真实客户端后，固定版本、平台、设备、语言、整理开关、词典、网络和输入框，
在同一音频路径重放同一批 WAV；如果使用扬声器到手机麦克风，voice-input 也必须走同一声学路径。
每条音频至少运行三次，保存屏幕录制、原始提交文字和时间戳。将逐条结果导出为 JSONL，包含
`id`、真实 `text`，以及可选的 `finish_ms`、应用版本、设备和模式。不能把未执行案例填写为空文本。

逐字模式按人工校对的录音转录评分；整理模式单独检查意思、否定、数字、术语是否保留，再看标点、
口水词和可读性。不能用逐字 CER 惩罚合法整理，或把漏掉关键事实称为“更简洁”。
通过条件是语义关键错误不多于实际竞品、噪声/长语音分组不明显退步，再比较完成时间和人工修改量。
在没有竞品结果时，只能报告内部 before/after。

## 数据授权

- [Google FLEURS](https://huggingface.co/datasets/google/fleurs)：CC-BY-4.0，真人朗读与人工转录；使用并修改为 PCM16/16kHz、加噪与拼接。
- [Steam Train Whistle](https://soundbible.com/2177-Steam-Train-Whistle.html)：作者 Daniel Simion（下载文件名为 Daniel Simon），CC-BY-3.0；
  [PyTorch 官方教程](https://docs.pytorch.org/audio/stable/tutorials/audio_data_augmentation_tutorial.html)提供该录音。
  本测试转换为单声道 16kHz 并重复叠加到语音，目标 SNR 10 dB，缩小整体幅度避免削波。
- 白噪声/静音由测试工具生成，随机种子固定；数据仅用于测试，不进入日常识别路径。
