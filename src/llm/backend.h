#pragma once

// The art director's mouth and ears.
//
// Three interchangeable backends: the Claude CLI, the Codex CLI and any
// OpenAI compatible HTTP endpoint. The CLI backends get image paths on disk
// and are expected to open them with their own tooling; the HTTP backend
// inlines them as base64. Everything else about the conversation is identical,
// so swapping backends cannot change the shape of the pipeline.

#include "core/json.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace rd {

// Replay answers every request from replies a previous run recorded, in the
// order it recorded them. No model, no network, byte-for-byte repeatable: it is
// how the prompt layer and the director's parsing get tested.
enum class LlmBackendKind : uint8_t { Disabled = 0, ClaudeCli, CodexCli, OpenAiApi, Replay };

const char*    llm_backend_name(LlmBackendKind k);
const char*    llm_backend_label(LlmBackendKind k);
LlmBackendKind llm_backend_from_name(std::string_view s);

struct LlmImage {
    std::string           label;   // what the picture shows, shown to the model
    std::filesystem::path path;
};

struct LlmRequest {
    std::string           system;
    std::string           user;
    std::vector<LlmImage> images;
    bool                  expect_json = true;
    int                   max_tokens  = 8192;
    float                 temperature = 0.2f;
    // Tag used in the transcript and in the saved prompt files.
    std::string           label = "request";
};

struct LlmResponse {
    bool        ok = false;
    std::string text;          // the model's reply, fences stripped when JSON was asked for
    Json        json;          // parsed when expect_json and parsing succeeded
    bool        json_ok = false;
    std::string error;
    std::string command;       // what was actually run, for the UI
    std::string raw;           // untouched backend output, for debugging
    double      seconds = 0.0;
    int         prompt_tokens = 0;
    int         completion_tokens = 0;
};

struct LlmConfig {
    LlmBackendKind kind = LlmBackendKind::ClaudeCli;

    // Claude CLI
    std::string claude_path  = "claude";
    // Pinned rather than left to the CLI: its default is whatever that install
    // was last set to, which made two runs of the same command ask two different
    // models - the recorded runs so far were answered by an old Opus. "default"
    // asks for the CLI default.
    std::string claude_model = "claude-opus-5-5";
    std::vector<std::string> claude_extra_args;

    // Codex CLI
    std::string codex_path   = "codex";
    std::string codex_model;
    std::vector<std::string> codex_extra_args;

    // OpenAI compatible HTTP
    std::string openai_base_url = "https://api.openai.com/v1";
    std::string openai_api_key;                // read from OPENAI_API_KEY when empty
    std::string openai_model    = "gpt-4o";
    bool        openai_send_images = true;

    // Replay: a run's reports/prompts folder. Each request labelled L takes the
    // next NN_L_text.txt in name order; see Pipeline::ask for the writing side.
    std::string replay_dir;

    int  timeout_seconds = 300;
    bool save_transcript = true;

    Json to_json() const;
    static LlmConfig from_json(const Json& j);
};

class ILlmBackend {
public:
    virtual ~ILlmBackend() = default;
    virtual LlmResponse complete(const LlmRequest& req,
                                 const std::atomic<bool>* cancel = nullptr) = 0;
    // Why the backend cannot run right now, if it cannot.
    virtual bool        available(std::string* reason = nullptr) const = 0;
    virtual const char* name() const = 0;
    virtual LlmBackendKind kind() const = 0;
    // The command line (or endpoint) this backend would use, for the UI.
    virtual std::string describe() const = 0;
};

std::unique_ptr<ILlmBackend> make_llm_backend(const LlmConfig& config);

// One entry per exchange, kept for the transcript panel and the written report.
struct LlmExchange {
    std::string stage;        // "segmentation naming", "budget allocation", ...
    std::string prompt;
    std::string reply;
    std::string error;
    std::vector<std::string> image_labels;
    double      seconds = 0.0;
    int         prompt_tokens = 0;
    int         completion_tokens = 0;
    bool        ok = false;
};

} // namespace rd
