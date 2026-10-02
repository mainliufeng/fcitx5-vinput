// Replays WAVs through the production ASR backends without taking microphone
// focus, altering user config, or invoking an unrelated cloud recognizer.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <sherpa-onnx/c-api/c-api.h>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/config/core_config.h"

#include "daemon/asr/backends/refining_backend.h"
#include "daemon/asr/backends/sherpa_offline_backend.h"
#include "daemon/asr/backends/sherpa_streaming_backend.h"

namespace {
using Clock = std::chrono::steady_clock;
using namespace vinput::daemon::asr;

double Milliseconds(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::vector<int16_t> ReadAudio(const std::string& path) {
  const std::unique_ptr<const SherpaOnnxWave, decltype(&SherpaOnnxFreeWave)> wave(
      SherpaOnnxReadWave(path.c_str()), SherpaOnnxFreeWave);
  if (!wave || wave->sample_rate != 16000) {
    throw std::runtime_error("Expected readable mono 16 kHz PCM WAV: " + path);
  }
  std::vector<int16_t> pcm;
  pcm.reserve(static_cast<std::size_t>(wave->num_samples));
  for (int i = 0; i < wave->num_samples; ++i) {
    pcm.push_back(
        static_cast<int16_t>(std::clamp(wave->samples[i] * 32768.0F, -32768.0F, 32767.0F)));
  }
  return pcm;
}

nlohmann::json Replay(AsrBackend& backend, const nlohmann::json& item) {
  auto pcm = ReadAudio(item.at("audio").get<std::string>());
  std::string error;
  const auto init_start = Clock::now();
  auto session = backend.CreateSession(&error);
  if (!session) {
    throw std::runtime_error(error);
  }
  const double init_ms = Milliseconds(init_start);
  const auto start = Clock::now();
  std::string final;
  std::vector<std::string> finals;
  double first_partial_audio_ms = -1;
  const auto drain = [&](double audio_ms) {
    for (const auto& event : session->PollEvents()) {
      if (event.kind == RecognitionEventKind::Error) {
        throw std::runtime_error(event.error);
      }
      if (event.kind == RecognitionEventKind::PartialText && !event.text.empty() &&
          first_partial_audio_ms < 0) {
        first_partial_audio_ms = audio_ms;
      }
      if (event.kind == RecognitionEventKind::FinalText) {
        final = event.text;
        finals.push_back(event.text);
      }
    }
  };
  constexpr std::size_t chunk_samples = 320; // Same 20 ms delivery as capture.
  for (std::size_t pos = 0; pos < pcm.size(); pos += chunk_samples) {
    const auto count = std::min(chunk_samples, pcm.size() - pos);
    if (!session->PushAudio(std::span<const int16_t>(pcm.data() + pos, count), &error)) {
      throw std::runtime_error(error);
    }
    drain(static_cast<double>(pos + count) / 16.0);
  }
  const auto finish_start = Clock::now();
  if (!session->Finish(&error)) {
    throw std::runtime_error(error);
  }
  const double finish_ms = Milliseconds(finish_start);
  drain(static_cast<double>(pcm.size()) / 16.0);
  return {{"id", item.at("id")},
          {"text", final},
          {"finals", finals},
          {"backend", backend.Describe().backend_id},
          {"duration_ms", static_cast<double>(pcm.size()) / 16.0},
          {"init_ms", init_ms},
          {"decode_ms", Milliseconds(start)},
          {"finish_ms", finish_ms},
          {"first_partial_audio_ms", first_partial_audio_ms}};
}
} // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr
        << "Usage: asr_replay MANIFEST.jsonl streaming|refined|offline|sensevoice vad|no-vad\n";
    return 2;
  }
  try {
    const std::string mode = argv[2];
    if (mode != "streaming" && mode != "refined" && mode != "offline" && mode != "sensevoice") {
      throw std::runtime_error("Unknown recognition mode");
    }
    const std::string vad_mode = argv[3];
    if (vad_mode != "vad" && vad_mode != "no-vad") {
      throw std::runtime_error("Unknown VAD mode");
    }
    CoreConfig config = LoadCoreConfig();
    const auto* active = ResolveActiveAsrProvider(config);
    const auto* local = active ? std::get_if<LocalAsrProvider>(active) : nullptr;
    if (!local) {
      throw std::runtime_error("Select a local ASR provider before replay");
    }
    LocalAsrProvider primary = *local;
    config.asr.vad.enabled = vad_mode == "vad";
    primary.model = "model.sherpa-onnx.x-asr-960ms-streaming-zipformer-transducer-zh-en-punct-int8";
    LocalAsrProvider offline = primary;
    offline.model = "model.sherpa-onnx.x-asr-zipformer-transducer-zh-en-punct-int8";
    if (mode == "sensevoice") {
      offline.model = "model.sherpa-onnx.sense-voice-zh-en-ja-ko-yue-int8";
      offline.hotwordsFile.clear();
    }
    std::string error;
    auto backend = mode == "offline" || mode == "sensevoice"
                       ? CreateSherpaOfflineBackend(config, offline, &error)
                       : CreateSherpaStreamingBackend(config, primary, &error);
    if (mode == "refined" && backend) {
      auto refine = CreateSherpaOfflineBackend(config, offline, &error);
      if (!refine) {
        throw std::runtime_error(error);
      }
      backend = CreateRefiningBackend(std::move(backend), std::move(refine), &error);
    }
    if (!backend) {
      throw std::runtime_error(error);
    }
    std::ifstream input(argv[1]);
    if (!input) {
      throw std::runtime_error("Cannot open manifest");
    }
    std::string line;
    while (std::getline(input, line)) {
      if (!line.empty()) {
        std::cout << Replay(*backend, nlohmann::json::parse(line)).dump() << std::endl;
      }
    }
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
