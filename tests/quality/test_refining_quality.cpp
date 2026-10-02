#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "daemon/asr/backends/refining_backend.h"

namespace {
using namespace vinput::daemon::asr;

class TextSession : public RecognitionSession {
public:
  TextSession(std::vector<std::string>& outputs, std::size_t& cursor)
      : outputs_(outputs), cursor_(cursor) {}
  bool PushAudio(std::span<const int16_t>, std::string*) override { return true; }
  bool Finish(std::string*) override {
    if (!finished_) {
      if (cursor_ < outputs_.size()) {
        events_.push_back({RecognitionEventKind::FinalText, outputs_[cursor_++], {}});
      }
      events_.push_back({RecognitionEventKind::Completed, {}, {}});
      finished_ = true;
    }
    return true;
  }
  void Cancel() override { events_.clear(); }
  std::vector<RecognitionEvent> PollEvents() override {
    std::vector<RecognitionEvent> result;
    result.swap(events_);
    return result;
  }

private:
  std::vector<std::string>& outputs_;
  std::size_t& cursor_;
  bool finished_ = false;
  std::vector<RecognitionEvent> events_;
};

class TextBackend : public AsrBackend {
public:
  explicit TextBackend(std::vector<std::string> outputs) : outputs_(std::move(outputs)) {}
  BackendDescriptor Describe() const override { return {}; }
  std::unique_ptr<RecognitionSession> CreateSession(std::string*) override {
    return std::make_unique<TextSession>(outputs_, cursor_);
  }

private:
  std::vector<std::string> outputs_;
  std::size_t cursor_ = 0;
};

bool Check(std::vector<std::string> refined, const std::string& expected) {
  std::string error;
  auto backend = CreateRefiningBackend(
      std::make_unique<TextBackend>(std::vector<std::string>{"complete streaming fallback"}),
      std::make_unique<TextBackend>(std::move(refined)), &error);
  auto session = backend->CreateSession(&error);
  std::vector<int16_t> pcm(16000 * 31, 100);
  if (!session->PushAudio(pcm, &error) || !session->Finish(&error)) {
    return false;
  }
  std::string final;
  int completed = 0;
  for (const auto& event : session->PollEvents()) {
    if (event.kind == RecognitionEventKind::FinalText) {
      final = event.text;
    }
    completed += event.kind == RecognitionEventKind::Completed ? 1 : 0;
  }
  session->Finish(&error);
  return final == expected && completed == 1 && session->PollEvents().empty();
}
} // namespace

int main() {
  if (!Check({"hello", "world"}, "hello world") || !Check({"你好", "世界"}, "你好世界") ||
      !Check({"hello ", "world"}, "hello world") ||
      !Check({"first chunk", ""}, "complete streaming fallback")) {
    std::cerr << "Long-dictation word boundary or fallback regression\n";
    return 1;
  }
  return 0;
}
