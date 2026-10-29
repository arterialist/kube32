#include "kubelet/kube_api_client.hpp"
#include "osal/socket.hpp"
#include "models/kube_models.hpp"

#include <algorithm>
#include <sstream>
#include <iostream>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <strings.h>
#include <new>

namespace kube32::kubelet {

HttpKubeClient::HttpKubeClient(std::string host, uint16_t port)
    : host_(std::move(host)), port_(port) {}

bool HttpKubeClient::is_chunked_transfer_encoding(std::string_view headers) {
    size_t cur = 0;
    while (cur < headers.size()) {
        size_t next_line = headers.find("\r\n", cur);
        size_t sep_len = 2;
        if (next_line == std::string_view::npos) {
            next_line = headers.find('\n', cur);
            sep_len = 1;
        }
        if (next_line == std::string_view::npos) {
            next_line = headers.size();
            sep_len = 0;
        }

        std::string_view line = headers.substr(cur, next_line - cur);
        size_t colon = line.find(':');
        if (colon != std::string_view::npos) {
            std::string_view key = line.substr(0, colon);
            std::string_view val = line.substr(colon + 1);

            while (!key.empty() && (key.front() == ' ' || key.front() == '\t')) key.remove_prefix(1);
            while (!key.empty() && (key.back() == ' ' || key.back() == '\t' || key.back() == '\r')) key.remove_suffix(1);

            constexpr std::string_view te_key = "transfer-encoding";
            if (key.size() == te_key.size()) {
                bool key_match = true;
                for (size_t i = 0; i < key.size(); ++i) {
                    if (std::tolower(static_cast<unsigned char>(key[i])) != te_key[i]) {
                        key_match = false;
                        break;
                    }
                }
                if (key_match) {
                    constexpr std::string_view chunked_token = "chunked";
                    if (val.size() >= chunked_token.size()) {
                        for (size_t i = 0; i <= val.size() - chunked_token.size(); ++i) {
                            bool token_match = true;
                            for (size_t j = 0; j < chunked_token.size(); ++j) {
                                if (std::tolower(static_cast<unsigned char>(val[i + j])) != chunked_token[j]) {
                                    token_match = false;
                                    break;
                                }
                            }
                            if (token_match) {
                                return true;
                            }
                        }
                    }
                }
            }
        }
        if (sep_len == 0) break;
        cur = next_line + sep_len;
    }
    return false;
}

std::string HttpKubeClient::decode_chunked_body(std::string_view raw_body) {
    std::string decoded;
    decoded.reserve(raw_body.size());

    size_t offset = 0;
    while (offset < raw_body.size()) {
        // Skip leading CRLF/LF between chunks if present
        while (offset < raw_body.size() && (raw_body[offset] == '\r' || raw_body[offset] == '\n')) {
            offset++;
        }
        if (offset >= raw_body.size()) break;

        size_t line_end = raw_body.find("\r\n", offset);
        size_t sep_len = 2;
        if (line_end == std::string_view::npos) {
            line_end = raw_body.find('\n', offset);
            sep_len = 1;
        }
        if (line_end == std::string_view::npos) {
            break;
        }

        std::string_view header_line = raw_body.substr(offset, line_end - offset);

        // Strip chunk extensions starting with ';'
        size_t semi = header_line.find(';');
        std::string_view hex_view = (semi != std::string_view::npos) ? header_line.substr(0, semi) : header_line;

        while (!hex_view.empty() && (hex_view.front() == ' ' || hex_view.front() == '\t')) {
            hex_view.remove_prefix(1);
        }
        while (!hex_view.empty() && (hex_view.back() == ' ' || hex_view.back() == '\t' || hex_view.back() == '\r')) {
            hex_view.remove_suffix(1);
        }

        if (hex_view.empty()) {
            break;
        }

        // Parse hex chunk size
        size_t chunk_size = 0;
        bool valid_hex = true;
        for (char c : hex_view) {
            chunk_size <<= 4;
            if (c >= '0' && c <= '9') {
                chunk_size |= static_cast<size_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                chunk_size |= static_cast<size_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                chunk_size |= static_cast<size_t>(c - 'A' + 10);
            } else {
                valid_hex = false;
                break;
            }
        }

        if (!valid_hex) {
            break;
        }

        // Terminal chunk
        if (chunk_size == 0) {
            break;
        }

        size_t data_start = line_end + sep_len;
        if (data_start >= raw_body.size()) {
            break;
        }

        size_t available = raw_body.size() - data_start;
        size_t to_copy = (chunk_size <= available) ? chunk_size : available;
        decoded.append(raw_body.data() + data_start, to_copy);

        if (chunk_size > available) {
            break;
        }

        offset = data_start + chunk_size;

        // Skip chunk trailer CRLF or LF
        if (offset + 1 < raw_body.size() && raw_body[offset] == '\r' && raw_body[offset + 1] == '\n') {
            offset += 2;
        } else if (offset < raw_body.size() && (raw_body[offset] == '\r' || raw_body[offset] == '\n')) {
            offset += 1;
        }
    }

    return decoded;
}

// Streams one exchange: the request goes out, the response head is parsed
// and the body (chunked or not) reaches `on_data` as it arrives, so no copy
// of the whole response is ever made. Returns the status, 0 when the server
// could not be reached or the response was malformed.
int HttpKubeClient::stream_request(const char* method, const std::string& path, const std::string& body,
                                   const std::function<bool(const char*, size_t)>& on_data, long* out_content_length) {
    if (out_content_length) *out_content_length = -1;
    osal::Socket sock;
    if (!sock.create(osal::SocketType::Tcp)) return 0;
    sock.set_timeout(3000, 3000);
    if (!sock.connect(host_.c_str(), port_)) return 0;

    std::string req;
    req.reserve(256 + body.size());
    req += method;
    req += " ";
    req += path;
    req += " HTTP/1.1\r\n";
    req += "Host: " + host_ + ":" + std::to_string(port_) + "\r\n";
    req += "User-Agent: kubelet/v1.33.0\r\n";
    req += "Accept: application/json\r\n";
    req += "Content-Type: application/json\r\n";
    req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    if (!token_.empty()) req += "Authorization: Bearer " + token_ + "\r\n";
    req += "Connection: close\r\n\r\n";
    req += body;
    if (!sock.send_all(req.data(), req.size())) return 0;
    std::string().swap(req);

    char buf[1024];
    std::string head;
    int status = 0;
    bool chunked = false;
    // Chunked decoding: bytes left in the current chunk, or reading a size line
    size_t chunk_left = 0;
    bool in_size_line = true;
    bool skip_crlf = false;
    char size_line[20];
    size_t size_len = 0;
    bool done = false;
    bool stop = false;
    auto feed = [&](const char* p, size_t len) {
        if (!chunked) {
            if (!on_data(p, len)) stop = true;
            return;
        }
        while (len > 0 && !done && !stop) {
            if (skip_crlf) {
                if (*p == '\n') skip_crlf = false;
                ++p; --len;
            } else if (in_size_line) {
                char c = *p++; --len;
                if (c == '\n') {
                    size_line[size_len] = '\0';
                    chunk_left = std::strtoul(size_line, nullptr, 16);
                    size_len = 0;
                    in_size_line = false;
                    if (chunk_left == 0) done = true;
                } else if (size_len + 1 < sizeof(size_line)) {
                    size_line[size_len++] = c;
                }
            } else {
                size_t take = len < chunk_left ? len : chunk_left;
                if (!on_data(p, take)) { stop = true; return; }
                p += take; len -= take; chunk_left -= take;
                if (chunk_left == 0) { in_size_line = true; skip_crlf = true; }
            }
        }
    };
    while (!done && !stop) {
        int n = sock.recv(buf, sizeof(buf));
        if (n <= 0) break;
        if (status != 0) {
            feed(buf, static_cast<size_t>(n));
            continue;
        }
        head.append(buf, static_cast<size_t>(n));
        size_t end = head.find("\r\n\r\n");
        if (end == std::string::npos) {
            if (head.size() > 4096) return 0;
            continue;
        }
        size_t sp = head.find(' ');
        if (sp == std::string::npos) return 0;
        status = std::atoi(head.c_str() + sp + 1);
        if (status <= 0) return 0;
        chunked = is_chunked_transfer_encoding(std::string_view(head.data(), end));
        if (out_content_length) {
            for (size_t cl = head.find('\n'); cl != std::string::npos && cl < end; cl = head.find('\n', cl + 1)) {
                if (head.size() - cl > 16 && strncasecmp(head.c_str() + cl + 1, "content-length:", 15) == 0) {
                    *out_content_length = std::strtol(head.c_str() + cl + 16, nullptr, 10);
                    break;
                }
            }
        }
        // Body bytes that arrived with the head
        if (head.size() > end + 4) feed(head.data() + end + 4, head.size() - end - 4);
        std::string().swap(head);
    }
    return status;
}

HttpKubeClient::HttpResponse HttpKubeClient::send_http_request(const char* method, const std::string& path, const std::string& body, bool keep_body) {
    HttpResponse resp;
    bool too_big = false;
    resp.status_code = stream_request(method, path, body, [&](const char* data, size_t len) {
        if (!keep_body) {
            constexpr size_t kKept = 160;
            if (resp.body.size() < kKept) resp.body.append(data, std::min(len, kKept - resp.body.size()));
            return true;
        }
        if (max_body_ && resp.body.size() + len > max_body_) {
            too_big = true;
            return false;
        }
        resp.body.append(data, len);
        return true;
    });
    if (too_big) {
        std::cout << "[KUBELET] " << method << " " << path.substr(0, 80) << ": response over " << max_body_ << " bytes, dropped" << std::endl;
        resp.status_code = 0;
        std::string().swap(resp.body);
    }
    return resp;
}

bool HttpKubeClient::register_node(const std::string& node_json, bool& out_conflict) {
    out_conflict = false;
    HttpResponse resp = send_http_request("POST", "/api/v1/nodes", node_json, false);
    if (resp.status_code == 201) {
        return true;
    }
    if (resp.status_code == 409) {
        out_conflict = true;
        return true;
    }
    return false;
}

bool HttpKubeClient::patch_node_status(const std::string& node_name, const std::string& patch_json) {
    HttpResponse resp = send_http_request("PATCH", "/api/v1/nodes/" + node_name, patch_json, false);
    if (resp.status_code != 200) {
        std::cout << "[KUBELET] node status patch failed: http " << resp.status_code
                  << (resp.body.empty() ? std::string() : " " + resp.body.substr(0, 120)) << std::endl;
    }
    return resp.status_code == 200;
}

bool HttpKubeClient::renew_node_lease(const std::string& node_name, uint32_t duration_sec, const std::string& renew_time_iso8601) {
    std::string path = "/apis/coordination.k8s.io/v1/namespaces/kube-node-lease/leases/" + node_name;
    // renewTime is omitted while the node has no wall clock; the API server
    // stamps the renewal on arrival.
    std::string lease_json = "{\"apiVersion\":\"coordination.k8s.io/v1\",\"kind\":\"Lease\","
                             "\"metadata\":{\"name\":\"" + node_name + "\",\"namespace\":\"kube-node-lease\"},"
                             "\"spec\":{\"holderIdentity\":\"" + node_name + "\","
                             "\"leaseDurationSeconds\":" + std::to_string(duration_sec) +
                             (renew_time_iso8601.empty() ? std::string() : ",\"renewTime\":\"" + renew_time_iso8601 + "\"") + "}}";

    HttpResponse resp = send_http_request("PUT", path, lease_json, false);
    if (resp.status_code == 200 || resp.status_code == 201) {
        return true;
    }
    if (resp.status_code == 404) {
        HttpResponse post_resp = send_http_request("POST", "/apis/coordination.k8s.io/v1/namespaces/kube-node-lease/leases", lease_json, false);
        return post_resp.status_code == 200 || post_resp.status_code == 201;
    }
    return false;
}

bool HttpKubeClient::list_pods(std::vector<models::KubePod>& out_pods) {
    HttpResponse resp = send_http_request("GET", "/api/v1/pods", "");
    if (resp.status_code != 200) {
        return false;
    }

    models::JsonValue doc;
    std::string err;
    if (!models::parse_json(resp.body, doc, err) || !doc.is_object()) {
        return false;
    }

    const models::JsonValue* items = doc.get("items");
    if (items && items->is_array()) {
        for (const auto& item : items->arr_val) {
            out_pods.push_back(models::parse_pod_manifest(item.to_string()));
        }
    }
    return true;
}

namespace {

// Splits a list response into its items as the bytes arrive: each element of
// the top-level "items" array is handed over as one JSON text.
class ListItemSplitter {
public:
    explicit ListItemSplitter(std::function<void(std::string&&)> on_item) : on_item_(std::move(on_item)) {}
    void feed(const char* p, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            const char c = p[i];
            const bool in_item = in_items_ && depth_ >= 3;
            if (in_item) item_.push_back(c);
            if (in_string_) {
                if (escape_) escape_ = false;
                else if (c == '\\') escape_ = true;
                else if (c == '"') {
                    in_string_ = false;
                    if (depth_ == 1 && !in_items_) last_key_ = key_;
                } else if (depth_ == 1 && key_.size() < 16) key_.push_back(c);
                continue;
            }
            switch (c) {
                case '"':
                    in_string_ = true;
                    key_.clear();
                    break;
                case '{':
                case '[':
                    if (c == '[' && depth_ == 1 && last_key_ == "items") in_items_ = true;
                    ++depth_;
                    if (in_items_ && depth_ == 3 && c == '{') { item_.clear(); item_.push_back('{'); }
                    break;
                case '}':
                case ']':
                    --depth_;
                    if (depth_ == 0) complete_ = true;
                    if (in_items_ && depth_ == 2 && c == '}') {
                        on_item_(std::move(item_));
                        item_.clear();
                    } else if (in_items_ && depth_ == 1) {
                        in_items_ = false;
                        last_key_.clear();
                    }
                    break;
                default:
                    break;
            }
        }
    }
    // The whole document arrived (its outermost value closed)
    bool complete() const { return complete_; }
private:
    std::function<void(std::string&&)> on_item_;
    bool complete_{false};
    std::string item_;
    std::string key_, last_key_;
    int depth_{0};
    bool in_string_{false};
    bool escape_{false};
    bool in_items_{false};
};

// Finds "kube32.io/wasm-base64":"<value>" in a stream and decodes the value
class InlineImageExtractor {
public:
    explicit InlineImageExtractor(std::vector<uint8_t>& out) : out_(out) {}
    void feed(const char* p, size_t n) {
        static const char kKey[] = "\"kube32.io/wasm-base64\":\"";
        for (size_t i = 0; i < n && !done_; ++i) {
            const char c = p[i];
            if (!in_value_) {
                if (c == kKey[matched_]) {
                    if (++matched_ == sizeof(kKey) - 1) in_value_ = true;
                } else {
                    matched_ = (c == kKey[0]) ? 1 : 0;
                }
                continue;
            }
            if (c == '"') { done_ = true; break; }
            if (c == '\\') continue;   // "\/" is an escaped '/'
            int v = (c >= 'A' && c <= 'Z') ? c - 'A' : (c >= 'a' && c <= 'z') ? c - 'a' + 26 : (c >= '0' && c <= '9') ? c - '0' + 52
                  : (c == '+' || c == '-') ? 62 : (c == '/' || c == '_') ? 63 : -1;
            if (v < 0) continue;   // padding, whitespace
            acc_ = (acc_ << 6) | static_cast<uint32_t>(v);
            bits_ += 6;
            if (bits_ >= 8) {
                bits_ -= 8;
                out_.push_back(static_cast<uint8_t>((acc_ >> bits_) & 0xFF));
            }
        }
    }
    bool found() const { return done_; }
private:
    std::vector<uint8_t>& out_;
    size_t matched_{0};
    bool in_value_{false};
    bool done_{false};
    uint32_t acc_{0};
    int bits_{0};
};

} // namespace

int KubeApiClient::list_items(const std::string& path, const std::function<void(std::string&&)>& on_item) {
    std::string body;
    const int status = get(path, body);
    if (status != 200) return status;
    models::JsonValue list;
    std::string err;
    if (!models::parse_json(body, list, err) || !list.is_object()) return 0;
    std::string().swap(body);
    if (const models::JsonValue* items = list.get("items"); items && items->is_array()) {
        for (const auto& item : items->arr_val) on_item(item.to_string());
    }
    return status;
}

int HttpKubeClient::list_items(const std::string& path, const std::function<void(std::string&&)>& on_item) {
    ListItemSplitter splitter([&](std::string&& item) { on_item(std::move(item)); });
    const int status = stream_request("GET", path, "", [&](const char* data, size_t len) {
        splitter.feed(data, len);
        return true;
    });
    return (status == 200 && !splitter.complete()) ? 0 : status;
}

bool HttpKubeClient::list_pods_raw(std::vector<std::pair<models::KubePod, std::string>>& out_pods_with_manifest) {
    std::string path = node_filter_.empty() ? std::string("/api/v1/pods?inlineImages=omit")
                                            : ("/api/v1/pods?fieldSelector=spec.nodeName%3D" + node_filter_ + "&inlineImages=omit");
    bool unreadable = false;
    const int status = list_items(path, [&](std::string&& item) {
        models::KubePod pod = models::parse_pod_manifest(item);
        // An item that does not parse (out of memory) must not read as a pod gone
        if (pod.name.empty()) unreadable = true;
        out_pods_with_manifest.emplace_back(std::move(pod), std::move(item));
    });
    // A list cut short (connection lost, node out of memory) is no list: the
    // pods missing from it must not be taken as deleted
    if (status != 200 || unreadable) {
        out_pods_with_manifest.clear();
        return false;
    }
    return true;
}

bool HttpKubeClient::get_pod_image(const std::string& ns, const std::string& name, std::vector<uint8_t>& out, std::string& why) {
    out.clear();
    InlineImageExtractor ex(out);
    bool reserved = false;
    long content_length = -1;
    int status = 0;
    try {
        status = stream_request("GET", "/api/v1/namespaces/" + ns + "/pods/" + name, "", [&](const char* data, size_t len) {
            if (!reserved) {
                reserved = true;
                // The image is most of the pod: three quarters of the response
                // bounds it, one allocation instead of repeated growth
                if (content_length > 0) out.reserve(static_cast<size_t>(content_length) / 4 * 3);
            }
            ex.feed(data, len);
            return !ex.found();
        }, &content_length);
    } catch (const std::bad_alloc&) {
        why = "not enough memory for the image of pod " + ns + "/" + name + " (" + std::to_string(content_length) + " byte pod)";
        std::vector<uint8_t>().swap(out);
        return false;
    }
    if (status != 200) {
        why = "pod " + ns + "/" + name + ": http " + std::to_string(status);
        out.clear();
        return false;
    }
    if (!ex.found() || out.empty()) {
        why = "pod " + ns + "/" + name + " has no kube32.io/wasm-base64 annotation";
        out.clear();
        return false;
    }
    return true;
}

bool HttpKubeClient::patch_pod_status(const std::string& ns, const std::string& name, const std::string& status_patch_json) {
    std::string path = "/api/v1/namespaces/" + ns + "/pods/" + name + "/status";
    HttpResponse resp = send_http_request("PATCH", path, status_patch_json, false);
    if (resp.status_code == 200) {
        return true;
    }
    // Also try PUT if PATCH returns 405
    if (resp.status_code == 405) {
        resp = send_http_request("PUT", path, status_patch_json, false);
        return resp.status_code == 200;
    }
    return false;
}


bool HttpKubeClient::create_event(const std::string& ns, const std::string& event_json) {
    HttpResponse resp = send_http_request("POST", "/api/v1/namespaces/" + ns + "/events", event_json, false);
    return resp.status_code == 201 || resp.status_code == 200;
}

bool HttpKubeClient::push_pod_logs(const std::string& ns, const std::string& name, const std::string& ndjson) {
    HttpResponse resp = send_http_request("POST", "/api/v1/namespaces/" + ns + "/pods/" + name + "/log", ndjson, false);
    return resp.status_code == 200;
}

int HttpKubeClient::get(const std::string& path, std::string& out_body) {
    HttpResponse resp = send_http_request("GET", path, "");
    out_body = std::move(resp.body);
    return resp.status_code;
}

bool HttpKubeClient::delete_pod(const std::string& ns, const std::string& name) {
    HttpResponse resp = send_http_request("DELETE", "/api/v1/namespaces/" + ns + "/pods/" + name + "?gracePeriodSeconds=0", "", false);
    return resp.status_code == 200 || resp.status_code == 404;
}

bool HttpKubeClient::push_metrics(const std::string& node_name, const std::string& sample_json) {
    HttpResponse resp = send_http_request("POST", "/apis/metrics.k8s.io/v1beta1/nodes/" + node_name, sample_json, false);
    return resp.status_code == 200;
}

} // namespace kube32::kubelet
