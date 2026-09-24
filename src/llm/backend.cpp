#include "llm/backend.h"

#include "core/log.h"
#include "core/paths.h"
#include "core/util.h"
#include "llm/http.h"
#include "llm/process.h"

#include <algorithm>
#include <cstdlib>
#include <map>

namespace rd {
namespace {

std::string compose_prompt(const LlmRequest& req, bool reference_images_by_path)
{
    std::string out;
    if (!req.system.empty()) {
        out += req.system;
        out += "\n\n";
    }
    if (reference_images_by_path && !req.images.empty()) {
        out += "Reference images on disk (read them before answering):\n";
        for (const LlmImage& img : req.images)
            out += "  - " + img.label + ": " + img.path.string() + "\n";
        out += "\n";
    }
    out += req.user;
    if (req.expect_json) {
        out += "\n\nReply with a single JSON object and nothing else. No prose before "
               "or after it, no markdown fence.";
    }
    return out;
}

void finish(LlmResponse& res, const LlmRequest& req)
{
    if (!req.expect_json || res.text.empty()) return;
    std::string error;
    res.json    = json_parse_lenient(res.text, error);
    res.json_ok = !res.json.is_null() && error.empty();
    if (!res.json_ok && res.ok) {
        res.ok    = false;
        res.error = error.empty() ? "the reply was not valid JSON" : error;
    }
}

// ---------------------------------------------------------------------------
class ClaudeCliBackend final : public ILlmBackend {
public:
    explicit ClaudeCliBackend(LlmConfig cfg) : cfg_(std::move(cfg)) {}

    bool available(std::string* reason) const override
    {
        if (which(cfg_.claude_path).empty()) {
            if (reason) *reason = "'" + cfg_.claude_path + "' is not on PATH";
            return false;
        }
        return true;
    }

    const char*    name() const override { return "Claude CLI"; }
    LlmBackendKind kind() const override { return LlmBackendKind::ClaudeCli; }

    std::string describe() const override
    {
        return format_command(cfg_.claude_path, build_args());
    }

    LlmResponse complete(const LlmRequest& req, const std::atomic<bool>* cancel) override
    {
        LlmResponse res;
        std::string reason;
        if (!available(&reason)) { res.error = reason; return res; }

        ProcessRequest pr;
        pr.executable      = cfg_.claude_path;
        pr.arguments       = build_args();
        pr.stdin_data      = compose_prompt(req, true);
        pr.timeout_seconds = cfg_.timeout_seconds;
        pr.cancel          = cancel;

        res.command = format_command(pr.executable, pr.arguments);
        const ProcessResult p = run_process(pr);
        res.seconds = p.seconds;
        res.raw     = p.out;

        if (!p.started)  { res.error = p.error; return res; }
        if (p.cancelled) { res.error = "cancelled"; return res; }
        if (p.timed_out) { res.error = format("timed out after %d s", cfg_.timeout_seconds); return res; }
        if (p.exit_code != 0) {
            // With --output-format json the CLI reports its own errors - an
            // unsupported model, an expired login - in the envelope on stdout
            // and leaves stderr empty, which logged "claude exited with 1: ".
            std::string why = trim(p.err);
            std::string envelope_error;
            const Json failed = json_parse_lenient(p.out, envelope_error);
            if (failed.is_object() && failed.contains("result"))
                why = json_get<std::string>(failed, "result", why);
            res.error = format("claude exited with %d: %s", p.exit_code, why.substr(0, 400).c_str());
            return res;
        }

        // --output-format json wraps the answer in an envelope.
        std::string parse_error;
        const Json envelope = json_parse_lenient(p.out, parse_error);
        if (envelope.is_object() && envelope.contains("result")) {
            res.text = json_get<std::string>(envelope, "result", "");
            const Json& usage = json_object_or_empty(envelope, "usage");
            res.prompt_tokens     = json_get<int>(usage, "input_tokens", 0);
            res.completion_tokens = json_get<int>(usage, "output_tokens", 0);
            if (json_get<bool>(envelope, "is_error", false)) {
                res.error = res.text.empty() ? "the CLI reported an error" : res.text;
                return res;
            }
        } else {
            res.text = trim(p.out);
        }

        if (res.text.empty()) {
            res.error = "empty reply";
            return res;
        }
        res.ok = true;
        finish(res, req);
        return res;
    }

private:
    std::vector<std::string> build_args() const
    {
        std::vector<std::string> args = {"-p", "--output-format", "json"};
        if (!cfg_.claude_model.empty() && cfg_.claude_model != "default") {
            args.push_back("--model");
            args.push_back(cfg_.claude_model);
        }
        for (const std::string& a : cfg_.claude_extra_args) args.push_back(a);
        return args;
    }

    LlmConfig cfg_;
};

// ---------------------------------------------------------------------------
class CodexCliBackend final : public ILlmBackend {
public:
    explicit CodexCliBackend(LlmConfig cfg) : cfg_(std::move(cfg)) {}

    bool available(std::string* reason) const override
    {
        if (which(cfg_.codex_path).empty()) {
            if (reason) *reason = "'" + cfg_.codex_path + "' is not on PATH";
            return false;
        }
        return true;
    }

    const char*    name() const override { return "Codex CLI"; }
    LlmBackendKind kind() const override { return LlmBackendKind::CodexCli; }

    std::string describe() const override
    {
        return format_command(cfg_.codex_path, build_args());
    }

    LlmResponse complete(const LlmRequest& req, const std::atomic<bool>* cancel) override
    {
        LlmResponse res;
        std::string reason;
        if (!available(&reason)) { res.error = reason; return res; }

        ProcessRequest pr;
        pr.executable      = cfg_.codex_path;
        pr.arguments       = build_args();
        pr.stdin_data      = compose_prompt(req, true);
        pr.timeout_seconds = cfg_.timeout_seconds;
        pr.cancel          = cancel;

        res.command = format_command(pr.executable, pr.arguments);
        const ProcessResult p = run_process(pr);
        res.seconds = p.seconds;
        res.raw     = p.out;

        if (!p.started)  { res.error = p.error; return res; }
        if (p.cancelled) { res.error = "cancelled"; return res; }
        if (p.timed_out) { res.error = format("timed out after %d s", cfg_.timeout_seconds); return res; }
        if (p.exit_code != 0) {
            res.error = format("codex exited with %d: %s", p.exit_code,
                               trim(p.err).substr(0, 400).c_str());
            return res;
        }

        // The CLI streams a running log; the answer is whatever JSON or text is
        // left once the noise is stripped. Being lenient here is cheaper than
        // tracking every change to an external tool's output format.
        res.text = trim(p.out);
        if (res.text.empty()) {
            res.error = "empty reply";
            return res;
        }
        res.ok = true;
        finish(res, req);
        return res;
    }

private:
    std::vector<std::string> build_args() const
    {
        std::vector<std::string> args;
        if (cfg_.codex_extra_args.empty()) {
            args = {"exec", "-"};
            if (!cfg_.codex_model.empty()) {
                args.push_back("--model");
                args.push_back(cfg_.codex_model);
            }
        } else {
            args = cfg_.codex_extra_args;
        }
        return args;
    }

    LlmConfig cfg_;
};

// ---------------------------------------------------------------------------
class OpenAiBackend final : public ILlmBackend {
public:
    explicit OpenAiBackend(LlmConfig cfg) : cfg_(std::move(cfg))
    {
        if (cfg_.openai_api_key.empty())
            if (const char* env = std::getenv("OPENAI_API_KEY")) cfg_.openai_api_key = env;
    }

    bool available(std::string* reason) const override
    {
        std::string why;
        if (!http_available(&why)) { if (reason) *reason = why; return false; }
        if (cfg_.openai_api_key.empty()) {
            if (reason) *reason = "no API key (set OPENAI_API_KEY or fill it in the settings)";
            return false;
        }
        return true;
    }

    const char*    name() const override { return "OpenAI API"; }
    LlmBackendKind kind() const override { return LlmBackendKind::OpenAiApi; }
    std::string    describe() const override
    {
        return "POST " + endpoint() + "  model=" + cfg_.openai_model;
    }

    LlmResponse complete(const LlmRequest& req, const std::atomic<bool>* cancel) override
    {
        LlmResponse res;
        std::string reason;
        if (!available(&reason)) { res.error = reason; return res; }

        Json body;
        body["model"]       = cfg_.openai_model;
        body["temperature"] = req.temperature;
        body["max_tokens"]  = req.max_tokens;
        if (req.expect_json) body["response_format"] = Json{{"type", "json_object"}};

        Json messages = Json::array();
        if (!req.system.empty())
            messages.push_back(Json{{"role", "system"}, {"content", req.system}});

        Json content = Json::array();
        content.push_back(Json{{"type", "text"}, {"text", compose_prompt(req, false)}});

        size_t attached = 0;
        if (cfg_.openai_send_images) {
            for (const LlmImage& img : req.images) {
                std::vector<uint8_t> bytes;
                if (!paths::read_file(img.path, bytes)) {
                    RD_WARN("cannot attach %s", img.path.string().c_str());
                    continue;
                }
                const std::string b64 = base64_encode(bytes.data(), bytes.size());
                content.push_back(Json{{"type", "text"}, {"text", "Image: " + img.label}});
                content.push_back(Json{
                    {"type", "image_url"},
                    {"image_url", Json{{"url", "data:image/png;base64," + b64}}}});
                ++attached;
            }
        }
        messages.push_back(Json{{"role", "user"}, {"content", content}});
        body["messages"] = messages;

        HttpRequest hr;
        hr.url     = endpoint();
        hr.method  = "POST";
        hr.body    = json_dump(body, -1);
        hr.headers = {{"Content-Type", "application/json"},
                      {"Authorization", "Bearer " + cfg_.openai_api_key}};
        hr.timeout_seconds = cfg_.timeout_seconds;
        hr.cancel  = cancel;

        res.command = format("POST %s (%zu images, %s payload)", hr.url.c_str(), attached,
                             paths::format_bytes(hr.body.size()).c_str());

        const HttpResponse http = http_request(hr);
        res.seconds = http.seconds;
        res.raw     = http.body;

        if (!http.ok) {
            res.error = http.error.empty() ? "request failed" : http.error;
            // Surface the API's own message, which is usually the useful part.
            std::string parse_error;
            const Json j = json_parse_lenient(http.body, parse_error);
            if (j.is_object()) {
                const Json& err = json_object_or_empty(j, "error");
                const std::string msg = json_get<std::string>(err, "message", "");
                if (!msg.empty()) res.error += ": " + msg;
            }
            return res;
        }

        std::string parse_error;
        const Json j = json_parse_lenient(http.body, parse_error);
        if (!j.is_object()) {
            res.error = "malformed response: " + parse_error;
            return res;
        }
        const Json& choices = json_array_or_empty(j, "choices");
        if (choices.empty()) {
            res.error = "the response contained no choices";
            return res;
        }
        const Json& message = json_object_or_empty(choices[0], "message");
        res.text = json_get<std::string>(message, "content", "");

        const Json& usage = json_object_or_empty(j, "usage");
        res.prompt_tokens     = json_get<int>(usage, "prompt_tokens", 0);
        res.completion_tokens = json_get<int>(usage, "completion_tokens", 0);

        if (res.text.empty()) {
            res.error = "empty reply";
            return res;
        }
        res.ok = true;
        finish(res, req);
        return res;
    }

private:
    std::string endpoint() const
    {
        std::string base = cfg_.openai_base_url;
        while (!base.empty() && base.back() == '/') base.pop_back();
        return base + "/chat/completions";
    }

    LlmConfig cfg_;
};

// ---------------------------------------------------------------------------
class ReplayBackend final : public ILlmBackend {
public:
    explicit ReplayBackend(const LlmConfig& c) : dir_(c.replay_dir) {}

    bool available(std::string* reason) const override
    {
        std::error_code ec;
        if (dir_.empty() || !std::filesystem::is_directory(dir_, ec)) {
            if (reason) *reason = "no replay folder: point it at a run's reports/prompts";
            return false;
        }
        for (const auto& e : std::filesystem::directory_iterator(dir_, ec))
            if (ends_with(e.path().filename().string(), "_text.txt")) return true;
        if (reason) *reason = "the replay folder holds no recorded replies (*_text.txt)";
        return false;
    }
    const char*    name() const override { return "Replay"; }
    LlmBackendKind kind() const override { return LlmBackendKind::Replay; }
    std::string    describe() const override { return "replaying " + dir_.string(); }

    LlmResponse complete(const LlmRequest& req, const std::atomic<bool>*) override
    {
        LlmResponse res;
        res.command = describe();
        // Recorded as NN_<label>_text.txt, NN being the iteration; sorting the
        // names puts them back in the order they were asked.
        const std::string suffix = "_" + slugify(req.label) + "_text.txt";
        std::vector<std::filesystem::path> matches;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(dir_, ec)) {
            const std::string f = e.path().filename().string();
            if (ends_with(f, suffix)) matches.push_back(e.path());
        }
        std::sort(matches.begin(), matches.end());

        const size_t k = asked_[req.label]++;
        if (k >= matches.size()) {
            res.error = format("no recorded reply #%zu for '%s' (%zu recorded)", k + 1,
                               req.label.c_str(), matches.size());
            return res;
        }
        std::string text;
        if (!paths::read_file(matches[k], text)) {
            res.error = "cannot read " + matches[k].string();
            return res;
        }
        res.text = text;
        res.raw  = text;
        res.ok   = true;
        finish(res, req);
        return res;
    }

private:
    std::filesystem::path             dir_;
    std::map<std::string, size_t>     asked_;
};

// ---------------------------------------------------------------------------
class DisabledBackend final : public ILlmBackend {
public:
    bool available(std::string* reason) const override
    {
        if (reason) *reason = "the director is switched off; knobs stay under manual control";
        return false;
    }
    const char*    name() const override { return "Disabled"; }
    LlmBackendKind kind() const override { return LlmBackendKind::Disabled; }
    std::string    describe() const override { return "no backend"; }

    LlmResponse complete(const LlmRequest&, const std::atomic<bool>*) override
    {
        LlmResponse res;
        res.error = "no LLM backend is selected";
        return res;
    }
};

} // namespace

// ---------------------------------------------------------------------------
const char* llm_backend_name(LlmBackendKind k)
{
    switch (k) {
    case LlmBackendKind::ClaudeCli: return "claude_cli";
    case LlmBackendKind::CodexCli:  return "codex_cli";
    case LlmBackendKind::OpenAiApi: return "openai_api";
    case LlmBackendKind::Replay:    return "replay";
    default:                        return "disabled";
    }
}

const char* llm_backend_label(LlmBackendKind k)
{
    switch (k) {
    case LlmBackendKind::ClaudeCli: return "Claude CLI";
    case LlmBackendKind::CodexCli:  return "Codex CLI";
    case LlmBackendKind::OpenAiApi: return "OpenAI API";
    case LlmBackendKind::Replay:    return "Replay";
    default:                        return "Disabled";
    }
}

LlmBackendKind llm_backend_from_name(std::string_view s)
{
    if (iequals(s, "claude_cli") || iequals(s, "claude")) return LlmBackendKind::ClaudeCli;
    if (iequals(s, "codex_cli")  || iequals(s, "codex"))  return LlmBackendKind::CodexCli;
    if (iequals(s, "openai_api") || iequals(s, "openai")) return LlmBackendKind::OpenAiApi;
    if (iequals(s, "replay"))                             return LlmBackendKind::Replay;
    return LlmBackendKind::Disabled;
}

Json LlmConfig::to_json() const
{
    Json j;
    j["kind"] = llm_backend_name(kind);

    Json c;
    c["path"]       = claude_path;
    c["model"]      = claude_model;
    c["extra_args"] = claude_extra_args;
    j["claude_cli"] = c;

    Json x;
    x["path"]       = codex_path;
    x["model"]      = codex_model;
    x["extra_args"] = codex_extra_args;
    j["codex_cli"]  = x;

    Json o;
    o["base_url"]    = openai_base_url;
    o["model"]       = openai_model;
    o["send_images"] = openai_send_images;
    // The key is deliberately not written to disk; it comes from the
    // environment or from the session.
    j["openai_api"]  = o;

    j["timeout_seconds"] = timeout_seconds;
    j["replay_dir"]      = replay_dir;
    j["save_transcript"] = save_transcript;
    return j;
}

LlmConfig LlmConfig::from_json(const Json& j)
{
    LlmConfig c;
    c.kind = llm_backend_from_name(json_get<std::string>(j, "kind", "claude_cli"));

    const Json& cc = json_object_or_empty(j, "claude_cli");
    c.claude_path  = json_get<std::string>(cc, "path", c.claude_path);
    // Earlier versions saved an empty model, meaning "no preference"; it still
    // does, and gets the pinned default rather than whatever the CLI is set to.
    const std::string saved_model = json_get<std::string>(cc, "model", "");
    if (!saved_model.empty()) c.claude_model = saved_model;
    c.claude_extra_args = json_get<std::vector<std::string>>(cc, "extra_args", {});

    const Json& xx = json_object_or_empty(j, "codex_cli");
    c.codex_path  = json_get<std::string>(xx, "path", c.codex_path);
    c.codex_model = json_get<std::string>(xx, "model", "");
    c.codex_extra_args = json_get<std::vector<std::string>>(xx, "extra_args", {});

    const Json& oo = json_object_or_empty(j, "openai_api");
    c.openai_base_url    = json_get<std::string>(oo, "base_url", c.openai_base_url);
    c.openai_model       = json_get<std::string>(oo, "model", c.openai_model);
    c.openai_send_images = json_get<bool>(oo, "send_images", c.openai_send_images);

    c.timeout_seconds = std::clamp(json_get<int>(j, "timeout_seconds", c.timeout_seconds), 10, 3600);
    c.replay_dir      = json_get<std::string>(j, "replay_dir", "");
    c.save_transcript = json_get<bool>(j, "save_transcript", c.save_transcript);
    return c;
}

std::unique_ptr<ILlmBackend> make_llm_backend(const LlmConfig& config)
{
    switch (config.kind) {
    case LlmBackendKind::ClaudeCli: return std::make_unique<ClaudeCliBackend>(config);
    case LlmBackendKind::CodexCli:  return std::make_unique<CodexCliBackend>(config);
    case LlmBackendKind::OpenAiApi: return std::make_unique<OpenAiBackend>(config);
    case LlmBackendKind::Replay:    return std::make_unique<ReplayBackend>(config);
    default:                        return std::make_unique<DisabledBackend>();
    }
}

} // namespace rd
