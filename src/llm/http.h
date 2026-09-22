#pragma once

// A deliberately small HTTPS client: one POST with JSON in and text out, which
// is all the OpenAI compatible backend needs. WinHTTP on Windows, libcurl via
// the command line elsewhere, so there is no extra dependency to vendor.

#include <atomic>
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

// True when the platform backend is compiled in and usable.
bool http_available(std::string* reason = nullptr);

} // namespace rd
