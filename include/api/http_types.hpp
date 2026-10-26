#pragma once

#include <string>
#include <string_view>
#include <cstdint>
#include <cstddef>

namespace kube32::api {

enum class HttpMethod {
    Get,
    Post,
    Put,
    Patch,
    Delete,
    Options,
    Head,
    Unknown
};

inline HttpMethod parse_http_method(std::string_view m) {
    if (m == "GET") return HttpMethod::Get;
    if (m == "POST") return HttpMethod::Post;
    if (m == "PUT") return HttpMethod::Put;
    if (m == "PATCH") return HttpMethod::Patch;
    if (m == "DELETE") return HttpMethod::Delete;
    if (m == "OPTIONS") return HttpMethod::Options;
    if (m == "HEAD") return HttpMethod::Head;
    return HttpMethod::Unknown;
}

inline const char* http_method_str(HttpMethod m) {
    switch (m) {
        case HttpMethod::Get: return "GET";
        case HttpMethod::Post: return "POST";
        case HttpMethod::Put: return "PUT";
        case HttpMethod::Patch: return "PATCH";
        case HttpMethod::Delete: return "DELETE";
        case HttpMethod::Options: return "OPTIONS";
        case HttpMethod::Head: return "HEAD";
        default: return "UNKNOWN";
    }
}

struct HttpRequest {
    HttpMethod method{HttpMethod::Unknown};
    std::string raw_uri;       // e.g. "/api/v1/nodes?timeout=32s"
    std::string path;          // e.g. "/api/v1/nodes"
    std::string query_string;  // e.g. "timeout=32s"
    std::string accept;
    std::string content_type;
    size_t content_length{0};
    bool invalid_content_length{false};
    std::string body;
    bool as_table{false};
};

struct HttpResponse {
    int status_code{200};
    std::string content_type{"application/json"};
    std::string body;
    bool is_chunked{false};
};

// URL Normalization helper: strips query string and trailing slashes
inline void normalize_uri(std::string_view raw_uri, std::string& out_path, std::string& out_query) {
    size_t qpos = raw_uri.find('?');
    if (qpos != std::string_view::npos) {
        out_path = std::string(raw_uri.substr(0, qpos));
        out_query = std::string(raw_uri.substr(qpos + 1));
    } else {
        out_path = std::string(raw_uri);
        out_query.clear();
    }

    // Strip trailing slashes unless path is "/"
    while (out_path.size() > 1 && out_path.back() == '/') {
        out_path.pop_back();
    }
    if (out_path.empty()) {
        out_path = "/";
    }
}

} // namespace kube32::api
