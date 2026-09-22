#pragma once

// A deliberately small HTTPS client: one POST with JSON in and text out, which
// is all the OpenAI compatible backend needs. WinHTTP on Windows, libcurl via
// the command line elsewhere, so there is no extra dependency to vendor.

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace rd {

struct HttpHeader {
    std::string name;
    std::string value;
};

struct HttpRequest {
    std::string             url;
    std::string             method = "POST";
    std::string             body;
    std::vector<HttpHeader> headers;
    int                     timeout_seconds = 300;
    const std::atomic<bool>* cancel = nullptr;
};

struct HttpResponse {
    bool        ok = false;      // transport succeeded and status is 2xx
    int         status = 0;
    std::string body;
    std::string error;
    double      seconds = 0.0;
};

HttpResponse http_request(const HttpRequest& req);

// A GET streamed to a file rather than into memory, because the one thing this
// is for - model checkpoints - is hundreds of megabytes.
//
// The bytes land in "<destination>.part" and are moved into place only once the
// transfer finished, so a cancelled or broken download can never be mistaken
// for a usable file. `progress` is called on the calling thread, often, with
// the total set to zero when the server did not say how big the body is.
struct HttpDownloadRequest {
    std::string              url;
    std::string              destination;
    int                      timeout_seconds = 3600;
    const std::atomic<bool>* cancel = nullptr;
    std::function<void(uint64_t received, uint64_t total)> progress;
};

struct HttpDownloadResult {
    bool        ok = false;
    int         status = 0;
    uint64_t    bytes = 0;      // what arrived
    uint64_t    expected = 0;   // what the server promised, 0 when it did not
    std::string error;
    double      seconds = 0.0;
};

HttpDownloadResult http_download(const HttpDownloadRequest& req);

// True when the platform backend is compiled in and usable.
bool http_available(std::string* reason = nullptr);

} // namespace rd
