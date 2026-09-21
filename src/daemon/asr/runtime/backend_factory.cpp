#include "daemon/asr/runtime/backend_factory.h"

#include "common/asr/model_manager.h"
#include "common/utils/debug_log.h"

#include "daemon/asr/backends/command_batch_backend.h"
#include "daemon/asr/backends/command_streaming_backend.h"

#include "config.h"
#if VINPUT_ENABLE_LOCAL_ASR
#include "daemon/asr/backends/refining_backend.h"
#include "daemon/asr/backends/sherpa_offline_backend.h"
#include "daemon/asr/backends/sherpa_streaming_backend.h"
#endif

namespace vinput::daemon::asr {

namespace {

bool IsStreamingCommandProvider(const CommandAsrProvider& provider) {
  static constexpr std::string_view kSuffix = ".streaming";
  const std::string_view id = provider.id;
  return id.size() >= kSuffix.size() && id.substr(id.size() - kSuffix.size()) == kSuffix;
}

#if VINPUT_ENABLE_LOCAL_ASR
// Attaches an optional second-pass offline model to a streaming backend.
//
// The second pass is best-effort by design: when the refine model is missing,
// is not an offline model, or fails to load, the streaming backend is returned
// unchanged instead of failing the session.
std::unique_ptr<AsrBackend> AttachSecondPass(const CoreConfig& config,
                                             const LocalAsrProvider& provider,
                                             std::unique_ptr<AsrBackend> primary,
                                             std::string* error) {
  ModelManager refine_mgr(ResolveModelBaseDir(config).string(), provider.refineModel);
  std::string refine_error;
  if (!refine_mgr.EnsureModels(&refine_error)) {
    debug::Log("vinput: second pass disabled for provider '%s': %s\n", provider.id.c_str(),
               refine_error.c_str());
    if (error) {
      error->clear();
    }
    return primary;
  }

  const ModelInfo refine_info = refine_mgr.GetModelInfo(&refine_error);
  const std::string refine_backend =
      refine_info.backend.empty() ? "sherpa-offline" : refine_info.backend;
  if (refine_backend != "sherpa-offline") {
    debug::Log("vinput: second pass disabled for provider '%s': refine model '%s' is '%s', "
               "only 'sherpa-offline' models can refine\n",
               provider.id.c_str(), provider.refineModel.c_str(), refine_backend.c_str());
    if (error) {
      error->clear();
    }
    return primary;
  }

  LocalAsrProvider refine_provider = provider;
  refine_provider.model = provider.refineModel;
  refine_provider.refineModel.clear();

  auto refine = CreateSherpaOfflineBackend(config, refine_provider, &refine_error);
  if (!refine) {
    debug::Log("vinput: second pass disabled for provider '%s': %s\n", provider.id.c_str(),
               refine_error.c_str());
    if (error) {
      error->clear();
    }
    return primary;
  }

  return CreateRefiningBackend(std::move(primary), std::move(refine), error);
}
#endif // VINPUT_ENABLE_LOCAL_ASR

std::unique_ptr<AsrBackend>
CreateLocalBackend(const CoreConfig& config, const LocalAsrProvider& provider, std::string* error) {
#if !VINPUT_ENABLE_LOCAL_ASR
  (void)config;
  (void)provider;
  if (error) {
    *error = "Local ASR support is disabled in this Lite build.";
  }
  return nullptr;
#else
  if (provider.model.empty()) {
    if (error) {
      *error = "Local ASR provider model is not configured.";
    }
    return nullptr;
  }

  ModelManager model_mgr(ResolveModelBaseDir(config).string(), provider.model);
  std::string model_error;
  if (!model_mgr.EnsureModels(&model_error)) {
    if (error) {
      *error = "Local ASR model check failed for provider '" + provider.id + "'";
      if (!model_error.empty()) {
        *error += ": " + model_error;
      } else {
        *error += ".";
      }
    }
    return nullptr;
  }

  ModelInfo model_info = model_mgr.GetModelInfo(&model_error);
  if (!model_error.empty()) {
    if (error) {
      *error = "Failed to read local ASR model metadata for provider '" + provider.id +
               "': " + model_error;
    }
    return nullptr;
  }

  const std::string backend_id = model_info.backend.empty() ? "sherpa-offline" : model_info.backend;
  if (backend_id == "sherpa-offline") {
    return CreateSherpaOfflineBackend(config, provider, error);
  }

  if (backend_id == "sherpa-streaming") {
    auto primary = CreateSherpaStreamingBackend(config, provider, error);
    if (!primary || provider.refineModel.empty()) {
      return primary;
    }
    return AttachSecondPass(config, provider, std::move(primary), error);
  }

  if (error) {
    *error =
        "Unsupported local ASR backend '" + backend_id + "' for provider '" + provider.id + "'.";
  }
  return nullptr;
#endif
}

} // namespace

std::unique_ptr<AsrBackend> CreateBackend(const CoreConfig& config, std::string* error) {
  const AsrProvider* provider = ResolveActiveAsrProvider(config);
  if (!provider) {
    if (error) {
      *error = "Active ASR provider not found.";
    }
    return nullptr;
  }

  if (const auto* local = std::get_if<LocalAsrProvider>(provider)) {
    return CreateLocalBackend(config, *local, error);
  }
  if (const auto* command = std::get_if<CommandAsrProvider>(provider)) {
    if (IsStreamingCommandProvider(*command)) {
      return CreateCommandStreamingBackend(*command, error);
    }
    return CreateCommandBatchBackend(*command, error);
  }

  if (error) {
    *error = "Unsupported ASR provider type: " + std::string(AsrProviderType(*provider));
  }
  return nullptr;
}

bool DescribeActiveBackend(const CoreConfig& config, BackendDescriptor* descriptor,
                           std::string* error) {
  auto backend = CreateBackend(config, error);
  if (!backend) {
    return false;
  }
  if (descriptor) {
    *descriptor = backend->Describe();
  }
  if (error) {
    error->clear();
  }
  return true;
}

} // namespace vinput::daemon::asr
