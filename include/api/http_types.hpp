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

// Reason phrase for a status line (RFC 9110 section 15). Codes without an
// entry get an empty phrase, which RFC 9112 allows.
inline const char* http_reason_phrase(int code) {
    switch (code) {
        case 100: return "Continue";
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 206: return "Partial Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 307: return "Temporary Redirect";
        case 308: return "Permanent Redirect";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 406: return "Not Acceptable";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 410: return "Gone";
        case 411: return "Length Required";
        case 412: return "Precondition Failed";
        case 413: return "Content Too Large";
        case 414: return "URI Too Long";
        case 415: return "Unsupported Media Type";
        case 416: return "Range Not Satisfiable";
        case 422: return "Unprocessable Content";
        case 428: return "Precondition Required";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        case 505: return "HTTP Version Not Supported";
        default: return "";
    }
}

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

    // Connection and application headers
    bool http11{true};
    bool keep_alive{true};
    bool accepts_gzip{false};
    bool chunked_body{false};
    std::string cookie;
    std::string if_none_match;
    std::string host;
    std::string request_id;
    std::string target_pod;   // X-Kube32-Pod, set by the ingress proxy
    std::string call_chain;   // X-Kube32-Call-Chain: pods serving the request that made this call
    std::string authorization;   // Authorization header, verbatim
    std::string client_ip;
};

struct HttpResponse {
    int status_code{200};
    std::string content_type{"application/json"};
    std::string body;
    bool is_chunked{false};
    // The handler moved the client socket to a long-lived owner (watch hub,
    // log follow); the server must not touch the connection again.
    bool socket_taken{false};
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
