// The VAD double supplies speech intervals. The production trimmer must keep
// their padded union in order, never repeat samples, and preserve disjoint gaps.
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <sherpa-onnx/c-api/c-api.h>
#include <utility>
#include <vector>

#include "daemon/asr/vad_trimmer.h"

struct SherpaOnnxVoiceActivityDetector {};
namespace {
struct FixtureState {
  SherpaOnnxVoiceActivityDetector detector;
  std::vector<std::pair<int, int>> segments;
  std::size_t cursor = 0;
};
FixtureState& State() {
  static FixtureState state;
  return state;
}
} // namespace

extern "C" {
const SherpaOnnxVoiceActivityDetector*
SherpaOnnxCreateVoiceActivityDetector(const SherpaOnnxVadModelConfig*, float) {
  return &State().detector;
}
void SherpaOnnxDestroyVoiceActivityDetector(const SherpaOnnxVoiceActivityDetector*) {}
void SherpaOnnxVoiceActivityDetectorReset(const SherpaOnnxVoiceActivityDetector*) {
  State().cursor = 0;
}
void SherpaOnnxVoiceActivityDetectorAcceptWaveform(const SherpaOnnxVoiceActivityDetector*,
                                                   const float*, int32_t) {}
void SherpaOnnxVoiceActivityDetectorFlush(const SherpaOnnxVoiceActivityDetector*) {}
int32_t SherpaOnnxVoiceActivityDetectorEmpty(const SherpaOnnxVoiceActivityDetector*) {
  return State().cursor == State().segments.size() ? 1 : 0;
}
const SherpaOnnxSpeechSegment*
SherpaOnnxVoiceActivityDetectorFront(const SherpaOnnxVoiceActivityDetector*) {
  const auto& interval = State().segments.at(State().cursor);
  return new SherpaOnnxSpeechSegment{
      .start = interval.first, .samples = nullptr, .n = interval.second};
}
void SherpaOnnxDestroySpeechSegment(const SherpaOnnxSpeechSegment* p) {
  delete p;
}
void SherpaOnnxVoiceActivityDetectorPop(const SherpaOnnxVoiceActivityDetector*) {
  ++State().cursor;
}
}

int main() {
  std::vector<float> input(72000);
  std::iota(input.begin(), input.end(), 0.0F);
  VadTrimmer trimmer;
  if (!trimmer.Init("test-double", 16000)) {
    return 1;
  }
  // First two protected intervals overlap; third is disjoint.
  State().segments = {{3200, 6400}, {12800, 6400}, {60000, 3200}};
  auto actual = trimmer.Trim(input, 16000);
  std::vector<float> expected(input.begin(), input.begin() + 24000);
  expected.insert(expected.end(), input.begin() + 55200, input.begin() + 68000);
  if (actual != expected || !trimmer.DetectedSpeech()) {
    std::cerr << "Protected speech intervals duplicated or reordered samples\n";
    return 1;
  }
  const std::vector<std::pair<int, int>> expected_ranges = {{0, 24000}, {55200, 68000}};
  if (trimmer.SpeechRanges() != expected_ranges) {
    return 1;
  }
  // A fully contained interval must not rewind the consumed position.
  State().segments = {{8000, 16000}, {12000, 1000}, {25000, 2000}};
  actual = trimmer.Trim(input, 16000);
  expected.assign(input.begin() + 3200, input.begin() + 31800);
  if (actual != expected) {
    return 1;
  }
  State().segments.clear();
  if (trimmer.Trim(input, 16000) != input || trimmer.DetectedSpeech() ||
      !trimmer.SpeechRanges().empty()) {
    return 1;
  }
  return 0;
}
