#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "daemon/asr/backends/refining_backend.h"
#include "daemon/asr/runtime/recognition_contract.h"

namespace {
using namespace vinput::daemon::asr;

struct TextState {
  std::vector<std::string> outputs;
  std::size_t cursor = 0;
};

class TextSession : public RecognitionSession {
public:
  explicit TextSession(std::shared_ptr<TextState> state) : state_(std::move(state)) {}
  bool PushAudio(std::span<const int16_t>, std::string*) override { return true; }
  bool Finish(std::string*) override {
    if (!finished_) {
      if (state_->cursor < state_->outputs.size()) {
        events_.push_back({.kind = RecognitionEventKind::FinalText,
                           .text = state_->outputs.at(state_->cursor++),
                           .error = {}});
      }
      events_.push_back({.kind = RecognitionEventKind::Completed, .text = {}, .error = {}});
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
  std::shared_ptr<TextState> state_;
  bool finished_ = false;
  std::vector<RecognitionEvent> events_;
};

class TextBackend : public AsrBackend {
public:
  explicit TextBackend(std::vector<std::string> outputs)
      : state_(std::make_shared<TextState>(TextState{.outputs = std::move(outputs), .cursor = 0})) {
  }
  [[nodiscard]] BackendDescriptor Describe() const override { return {}; }
  std::unique_ptr<RecognitionSession> CreateSession(std::string*) override {
    return std::make_unique<TextSession>(state_);
  }

private:
  std::shared_ptr<TextState> state_;
};

bool Check(std::vector<std::string> refined, const std::string& expected) {
  std::string error;
  auto backend = CreateRefiningBackend(
      std::make_unique<TextBackend>(std::vector<std::string>{"complete streaming fallback"}),
      std::make_unique<TextBackend>(std::move(refined)), &error);
  auto session = backend->CreateSession(&error);
  std::vector<int16_t> pcm(std::size_t{16000} * 31, 100);
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
      !Check({"hello ", "world"}, "hello world") || !Check({"hello.", "World"}, "hello. World") ||
      !Check({"first chunk", ""}, "complete streaming fallback")) {
    std::cerr << "Long-dictation word boundary or fallback regression\n";
    return 1;
  }
  return 0;
}
