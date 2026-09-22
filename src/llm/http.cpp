#include "llm/http.h"

#include "core/log.h"
#include "core/paths.h"
#include "core/util.h"
#include "llm/process.h"

#include <algorithm>

#if defined(RD_PLATFORM_WINDOWS)
#  include <windows.h>
#  include <winhttp.h>
#endif

namespace rd {
namespace {

struct ParsedUrl {
    bool        ok = false;
    bool        https = true;
    std::string host;
    int         port = 443;
    std::string path = "/";
};

ParsedUrl parse_url(const std::string& url)
{
    ParsedUrl out;
    size_t pos = url.find("://");
    if (pos == std::string::npos) return out;

    const std::string scheme = to_lower(url.substr(0, pos));
    if (scheme == "http")       { out.https = false; out.port = 80; }
    else if (scheme == "https") { out.https = true;  out.port = 443; }
    else return out;

    const std::string rest = url.substr(pos + 3);
    const size_t slash = rest.find('/');
    std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    out.path = slash == std::string::npos ? "/" : rest.substr(slash);

    const size_t colon = authority.rfind(':');
    if (colon != std::string::npos && authority.find(']') == std::string::npos) {
        out.host = authority.substr(0, colon);
        out.port = std::atoi(authority.c_str() + colon + 1);
    } else {
        out.host = authority;
    }
    out.ok = !out.host.empty();
    return out;
}

} // namespace

#if defined(RD_PLATFORM_WINDOWS)

namespace {

std::wstring widen(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
    return out;
}

struct Handle {
    HINTERNET h = nullptr;
    explicit Handle(HINTERNET handle = nullptr) : h(handle) {}
    ~Handle() { if (h) WinHttpCloseHandle(h); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    operator HINTERNET() const { return h; }
};

} // namespace

bool http_available(std::string*) { return true; }

HttpResponse http_request(const HttpRequest& req)
{
    HttpResponse res;
    Stopwatch watch;

    const ParsedUrl url = parse_url(req.url);
    if (!url.ok) {
        res.error = "cannot parse url: " + req.url;
        return res;
    }

    Handle session(WinHttpOpen(L"RetopoDirector/" RD_VERSION_STRING,
                               WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        res.error = format("WinHttpOpen failed (%lu)", GetLastError());
        return res;
    }

    const DWORD timeout_ms = DWORD(std::max(1, req.timeout_seconds) * 1000);
    WinHttpSetTimeouts(session, 15000, 15000, int(timeout_ms), int(timeout_ms));

    Handle connect(WinHttpConnect(session, widen(url.host).c_str(),
                                  INTERNET_PORT(url.port), 0));
    if (!connect) {
        res.error = format("WinHttpConnect failed (%lu)", GetLastError());
        return res;
    }

    Handle request(WinHttpOpenRequest(connect, widen(req.method).c_str(),
                                      widen(url.path).c_str(), nullptr,
                                      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                      url.https ? WINHTTP_FLAG_SECURE : 0));
    if (!request) {
        res.error = format("WinHttpOpenRequest failed (%lu)", GetLastError());
        return res;
    }

    std::wstring headers;
    for (const HttpHeader& h : req.headers)
        headers += widen(h.name + ": " + h.value + "\r\n");

    const BOOL sent = WinHttpSendRequest(
        request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
        headers.empty() ? 0 : DWORD(-1),
        req.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(req.body.data()),
        DWORD(req.body.size()), DWORD(req.body.size()), 0);
    if (!sent) {
        res.error = format("WinHttpSendRequest failed (%lu)", GetLastError());
        return res;
    }

    if (!WinHttpReceiveResponse(request, nullptr)) {
        res.error = format("WinHttpReceiveResponse failed (%lu)", GetLastError());
        return res;
    }

    DWORD status = 0, status_size = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                        WINHTTP_NO_HEADER_INDEX);
    res.status = int(status);

    for (;;) {
        if (req.cancel && req.cancel->load()) {
            res.error = "cancelled";
            return res;
        }
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;

        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) break;
        chunk.resize(read);
        res.body += chunk;
    }

    res.ok      = res.status >= 200 && res.status < 300;
    res.seconds = watch.seconds();
    if (!res.ok && res.error.empty())
        res.error = format("http %d", res.status);
    return res;
}

#else   // POSIX: shell out to curl rather than linking another library.

bool http_available(std::string* reason)
{
    if (!which("curl").empty()) return true;
    if (reason) *reason = "curl is not on PATH";
    return false;
}

HttpResponse http_request(const HttpRequest& req)
{
    HttpResponse res;
    Stopwatch watch;

    std::string reason;
    if (!http_available(&reason)) {
        res.error = reason;
        return res;
    }

    ProcessRequest pr;
    pr.executable = "curl";
    pr.arguments  = {"-sS", "-X", req.method, "--data-binary", "@-",
                     "-w", "\n%{http_code}", req.url};
    for (const HttpHeader& h : req.headers) {
        pr.arguments.push_back("-H");
        pr.arguments.push_back(h.name + ": " + h.value);
    }
    pr.stdin_data      = req.body;
    pr.timeout_seconds = req.timeout_seconds;
    pr.cancel          = req.cancel;

    const ProcessResult p = run_process(pr);
    if (!p.started) { res.error = p.error; return res; }
    if (p.timed_out) { res.error = "request timed out"; return res; }

    // The status code was appended on its own final line.
    const size_t nl = p.out.find_last_of('\n');
    if (nl == std::string::npos) {
        res.error = p.err.empty() ? "malformed curl response" : p.err;
        return res;
    }
    res.body   = p.out.substr(0, nl);
    res.status = std::atoi(p.out.c_str() + nl + 1);
    res.ok     = res.status >= 200 && res.status < 300;
    if (!res.ok) res.error = format("http %d", res.status);
    res.seconds = watch.seconds();
    return res;
}

#endif

} // namespace rd
