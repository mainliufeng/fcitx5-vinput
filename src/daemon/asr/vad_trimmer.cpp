#include "vad_trimmer.h"

#include <algorithm>
#include <cstdio>
#include <sherpa-onnx/c-api/c-api.h>

VadTrimmer::VadTrimmer() = default;

VadTrimmer::~VadTrimmer() {
  Shutdown();
}

bool VadTrimmer::Init(const std::string& model_path, int sample_rate, const std::string& provider,
                      const VadTrimParams& params, std::string* error) {
  if (vad_)
    return true;

  params_ = params;
  SherpaOnnxVadModelConfig config = {};
  config.silero_vad.model = model_path.c_str();
  config.silero_vad.threshold = params_.threshold;
  config.silero_vad.min_silence_duration = params_.min_silence_duration;
  config.silero_vad.min_speech_duration = params_.min_speech_duration;
  config.silero_vad.window_size = 512;
  config.silero_vad.max_speech_duration = 0.0f;
  config.sample_rate = sample_rate;
  config.num_threads = 1;
  config.provider = provider.c_str();
  config.debug = 0;

  vad_ = SherpaOnnxCreateVoiceActivityDetector(&config, 30.0f);
  if (!vad_) {
    if (error) {
      *error = "failed to create VAD from '" + model_path + "'";
    }
    return false;
  }

  sample_rate_ = sample_rate;
  fprintf(stderr,
          "vinput: VAD initialized from '%s' threshold=%.2f min_speech=%.2f "
          "min_silence=%.2f pad_ms=%d\n",
          model_path.c_str(), params_.threshold, params_.min_speech_duration,
          params_.min_silence_duration, params_.speech_pad_ms);
  return true;
}

std::vector<float> VadTrimmer::Trim(const std::vector<float>& samples, int /*sample_rate*/) {
  detected_speech_ = false;
  speech_ranges_.clear();
  if (!vad_ || samples.empty())
    return samples;

  SherpaOnnxVoiceActivityDetectorReset(vad_);

  // Feed audio in window_size chunks
  const int window_size = 512;
  const int n = static_cast<int>(samples.size());
  int offset = 0;
  for (; offset + window_size <= n; offset += window_size) {
    SherpaOnnxVoiceActivityDetectorAcceptWaveform(vad_, samples.data() + offset, window_size);
  }
  if (offset < n) {
    std::vector<float> padded_tail(window_size, 0.0f);
    const int remaining = n - offset;
    for (int i = 0; i < remaining; ++i) {
      padded_tail[i] = samples[offset + i];
    }
    SherpaOnnxVoiceActivityDetectorAcceptWaveform(vad_, padded_tail.data(), window_size);
  }
  SherpaOnnxVoiceActivityDetectorFlush(vad_);

  const int padding_samples = std::max(
      0, static_cast<int>(static_cast<long long>(params_.speech_pad_ms) * sample_rate_ / 1000));
  std::vector<float> result;
  int first_start = -1;
  int last_end = -1;
  while (!SherpaOnnxVoiceActivityDetectorEmpty(vad_)) {
    const SherpaOnnxSpeechSegment* seg = SherpaOnnxVoiceActivityDetectorFront(vad_);
    if (seg && seg->n > 0) {
      int start = std::max(0, static_cast<int>(seg->start) - padding_samples);
      int end =
          std::min(n, static_cast<int>(seg->start) + static_cast<int>(seg->n) + padding_samples);
      // Adjacent speech segments may have overlapping protective padding.
      // Consume each original sample once; duplicating a boundary can make
      // the recognizer repeat syllables or insert words around a short pause.
      start = std::max(start, last_end);
      if (first_start < 0) {
        first_start = start;
      }
      if (end > start) {
        if (!speech_ranges_.empty() && start <= speech_ranges_.back().second) {
          speech_ranges_.back().second = end;
        } else {
          speech_ranges_.emplace_back(start, end);
        }
        last_end = end;
        result.insert(result.end(), samples.begin() + start, samples.begin() + end);
      }
    }
    if (seg) {
      SherpaOnnxDestroySpeechSegment(seg);
    }
    SherpaOnnxVoiceActivityDetectorPop(vad_);
  }

  detected_speech_ = !result.empty();
  if (result.empty()) {
    fprintf(stderr, "vinput: VAD found no speech, returning original audio\n");
    return samples;
  }

  const int leading_removed = first_start > 0 ? first_start : 0;
  const int trailing_removed = last_end >= 0 ? (n - last_end) : 0;
  fprintf(stderr,
          "vinput: VAD trimmed %d -> %zu samples leading_removed_ms=%d "
          "trailing_removed_ms=%d pad_ms=%d\n",
          n, result.size(), leading_removed * 1000 / sample_rate_,
          trailing_removed * 1000 / sample_rate_, params_.speech_pad_ms);
  return result;
}

bool VadTrimmer::Available() const {
  return vad_ != nullptr;
}

bool VadTrimmer::DetectedSpeech() const {
  return detected_speech_;
}

const std::vector<std::pair<int, int>>& VadTrimmer::SpeechRanges() const {
  return speech_ranges_;
}

void VadTrimmer::Shutdown() {
  if (vad_) {
    SherpaOnnxDestroyVoiceActivityDetector(vad_);
    vad_ = nullptr;
  }
}
