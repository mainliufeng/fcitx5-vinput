---
title: ASR
description: "Speech recognition: local models, cloud providers, and hotwords."
---

## Concepts

ASR (Automatic Speech Recognition) converts speech into text — the first step in the voice input pipeline.

```
Microphone → [ASR] → Raw text → (optional) Scene + LLM rewriting → Final text
```

Vinput provides three complementary ASR mechanisms:

- **Local models** — Offline recognition powered by [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx). No network required, privacy-friendly, low latency.
- **Cloud providers** — Third-party ASR APIs (Doubao, Aliyun Bailian, ElevenLabs, OpenAI, etc.). Typically better accuracy, but requires network and API keys.
- **Hotwords** — Domain-specific vocabulary to improve recognition of proper nouns with local models (supported by some models).

Local models and cloud providers are **mutually exclusive** — switch between them at runtime from the command palette (`Shift_R` → `/asr`). Hotwords take effect when a local model is active.

Local models can additionally be combined into a **two-pass relay**: a streaming model keeps text appearing while you speak, and a second offline model re-decodes the same audio at release to finalise it (see “Second-pass refinement” below).

## Local models

### Concept

A local model is a set of sherpa-onnx compatible model files that run entirely offline. Each model has its own language, type, and size. Only one can be active at a time.

Corresponding config:

```json
{
  "asr": {
    "providers": [
      {
        "id": "sherpa-onnx",
        "type": "local",
        "model": "model.sherpa-onnx.sense-voice-zh-en-ja-ko-yue-int8",
        "timeout_ms": 15000
      }
    ]
  }
}
```

### GUI

In Vinput GUI, go to **Resources → Models**:

- **Available models** list: click **Download** to install
- **Installed models** list: click **Use** to activate, **Remove** to uninstall

### CLI

```bash
vinput model list               # List installed models
vinput model list -a            # List available remote models
vinput model add <name>         # Download and install
vinput model use <name>         # Activate
vinput model remove <name>      # Uninstall
vinput model info <name>        # View details
```

## Second-pass refinement

### Concept

Local models force a trade-off between two paths:

- **Streaming models** — text appears while you speak, low latency; but they are usually smaller, and proper nouns or mixed Chinese/English are error-prone.
- **Offline models** — transcribe the whole utterance with clearly better accuracy; but nothing appears until you stop.

Second-pass refinement turns them into a **relay**: a streaming model keeps the live preview during recording (pass 1), and at release a second offline model re-decodes the **same audio** once (pass 2). Pass 2 decides the committed text.

```
Audio ─┬─ pass 1: streaming model ──→ live preedit (while speaking)
       └─ pass 2: offline model   ──→ committed text (+150–250 ms after release)
```

The second pass is emitted as the **last `FinalText`**, so it replaces the streaming result and still flows through the normal scene / LLM rewriting.

### Configuration

Add `refine_model` to the local provider, pointing at an **installed offline model**:

```json
{
  "asr": {
    "providers": [
      {
        "id": "sherpa-onnx",
        "type": "local",
        "model": "model.sherpa-onnx.x-asr-960ms-streaming-zipformer-transducer-zh-en-punct-int8",
        "refine_model": "model.sherpa-onnx.x-asr-zipformer-transducer-zh-en-punct-int8",
        "timeout_ms": 15000
      }
    ]
  }
}
```

Leaving `refine_model` unset (or empty) keeps the previous behaviour: streaming only.

### CLI

```bash
vinput refine get               # Show the current refinement model
vinput refine set <name>        # Set it (accepts the short ID and resolves it)
vinput refine clear             # Disable refinement, back to streaming only
```

### Choosing a refinement model

1. **It must be an offline (`sherpa-offline`) model.** Passing a streaming model is rejected and the session quietly falls back to streaming only.
2. **Prefer a model from the same family as pass 1, and one that handles both languages.** This matters most for mixed Chinese/English speech:

| Refinement model | Result |
|---|---|
| Same-family offline model (e.g. the offline X-ASR export) | ✅ Chinese, English, punctuation and casing all keep or improve |
| A Chinese-leaning single-language model (e.g. SenseVoice) | ❌ Corrupts English that pass 1 already got right: `repo`→`RAIPPLE`, `prompt`→`PROMT`, `push`→`布置`, and upper-cases all English |

The precondition for a second pass is that **it must not be worse than the first**. A Chinese model with weak English vocabulary will actively damage correct English words.

### Fallback behaviour

The second pass is a best-effort optimisation, never a correctness dependency:

- If the refinement model is missing, fails to load, or returns no text, the streaming result is kept silently.
- No error in the second pass can drop an utterance.

To confirm the second pass is running, enable debug logging:

```bash
journalctl --user -u vinput-daemon | grep -E 'pass 1|pass 2' | tail -4
```

Example output:

```
vinput:   pass 1 (streaming): 我平时会 skill 这个功能， 然后把它 push 到 repo 里边
vinput:   pass 2 (refined):   我平时会 skill 这个功能， 然后把它 push 到 repo 里面。
```

### Cost

- **Memory**: the refinement model stays resident, roughly +130 MB for an int8 offline X-ASR; zero if unset.
- **Latency**: +150–250 ms after release (one full re-decode).
- **First-word latency is unchanged** — pass 1 is still streaming.

## Cloud providers

### Concept

A cloud provider is an external script that receives an audio stream, calls a third-party ASR API, and returns recognized text. Each provider has its own environment variable config (API key, URL, etc.).

Providers come in two modes:
- **Non-streaming** — Sends audio after recording ends, waits for complete result
- **Streaming** — Recognizes in real time as you speak, returns intermediate results

Corresponding config:

```json
{
  "asr": {
    "active_provider": "provider.doubaoime.streaming",
    "providers": [
      {
        "id": "provider.bailian.streaming",
        "type": "command",
        "command": "python3",
        "args": ["~/.local/share/vinput/providers/bailian/streaming"],
        "env": {
          "VINPUT_ASR_API_KEY": "your-api-key",
          "VINPUT_ASR_MODEL": "qwen3-asr-flash-realtime"
        },
        "timeout_ms": 60000
      }
    ]
  }
}
```

### GUI

In Vinput GUI, go to **Resources → ASR Providers**:

- Click **Install** to download a provider script
- After installation, go to the **Control** page to select and edit provider environment variables (e.g. API key)

### CLI

```bash
vinput provider list -a        # List available remote providers
vinput provider add <id>       # Install
vinput provider use <id>       # Switch to this provider
vinput provider edit <id>      # Edit config (environment variables)
vinput provider remove <id>    # Uninstall
```

### Available providers

| Provider | Mode | Description |
|----------|------|-------------|
| Doubao (non-streaming) | Non-streaming | Doubao / Volcengine fast file recognition |
| ElevenLabs | Non-streaming / Streaming | ElevenLabs speech-to-text API |
| Aliyun Bailian | Non-streaming / Streaming | Qwen3-ASR via OpenAI-compatible / Realtime API |
| Doubao (streaming) | Streaming | Doubao ASR Realtime via AI Gateway |
| Doubao IME (streaming) | Streaming | Unofficial Doubao IME real-time protocol |
| OpenAI-compatible | Non-streaming / Streaming | OpenAI `/v1/audio/transcriptions` or Realtime WebSocket |

## Hotwords

### Concept

A hotword file is a text file with one term per line, used to boost recognition accuracy for specific vocabulary with local models. Typical use cases: names, brand names, technical terms.

```text
OpenAI
speech recognition:2.0
deep learning:3.5
```

For models supporting per-entry weights, append `:<weight>` directly to the term. Do not add spaces around the colon: use `term:2.0`, not `term :2.0` or `term: 2.0`. Models without per-entry weights ignore the weight suffix and use only the term.

Not all models support hotwords — the model list indicates support.

### GUI

In Vinput GUI, go to the **Hotwords** tab to edit.

### CLI

```bash
vinput hotword get              # View current hotword file path
vinput hotword set <path>       # Set hotword file
vinput hotword edit             # Open hotword file in editor
vinput hotword clear            # Clear hotword config
```
