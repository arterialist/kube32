#include "kubelet/kube_api_client.hpp"
#include "osal/socket.hpp"
#include "osal/system.hpp"
#include "models/kube_models.hpp"

#include <algorithm>
#include <sstream>
#include <iostream>
#include <cstdio>
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

namespace {

// A request path put together on the stack
class PathBuf {
public:
    PathBuf& add(std::string_view part) {
        if (len_ + part.size() >= sizeof(buf_)) {
            ok_ = false;
            return *this;
        }
        std::memcpy(buf_ + len_, part.data(), part.size());
        len_ += part.size();
        return *this;
    }
    // Empty when the path did not fit: the request is not sent
    std::string_view view() const { return ok_ ? std::string_view(buf_, len_) : std::string_view(); }

private:
    char buf_[384];
    size_t len_{0};
    bool ok_{true};
};

// The value of header `name` in a response head, empty when absent
std::string_view header_value(std::string_view head, std::string_view name) {
    for (size_t line = head.find('\n'); line != std::string_view::npos; line = head.find('\n', line + 1)) {
        std::string_view rest = head.substr(line + 1);
        if (rest.size() <= name.size() || rest[name.size()] != ':' || strncasecmp(rest.data(), name.data(), name.size()) != 0) continue;
        rest.remove_prefix(name.size() + 1);
        while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) rest.remove_prefix(1);
        const size_t end = rest.find_first_of("\r\n");
        return end == std::string_view::npos ? rest : rest.substr(0, end);
    }
    return std::string_view();
}

} // namespace

// The request head and the response head are put together in buffers on the
// stack and the body reaches `on_data` as it arrives: an exchange takes
// nothing from the heap (lwIP's own buffers aside).
int HttpKubeClient::stream_request(const char* method, std::string_view path, std::string_view body,
                                   mem::FunctionRef<bool(const char*, size_t)> on_data, long* out_content_length) {
    if (out_content_length) *out_content_length = -1;
    if (path.empty()) return 0;
    char buf[1024];
    int head_len = snprintf(buf, sizeof(buf),
                            "%s %.*s HTTP/1.1\r\nHost: %s:%u\r\nUser-Agent: kubelet/v1.33.0\r\nAccept: application/json\r\n"
                            "Content-Type: application/json\r\nContent-Length: %u\r\n%s%s%sConnection: close\r\n\r\n",
                            method, static_cast<int>(path.size()), path.data(), host_.c_str(), static_cast<unsigned>(port_),
                            static_cast<unsigned>(body.size()), token_.empty() ? "" : "Authorization: Bearer ", token_.c_str(),
                            token_.empty() ? "" : "\r\n");
    if (head_len <= 0 || static_cast<size_t>(head_len) >= sizeof(buf)) return 0;

    osal::Socket sock;
    if (!sock.create(osal::SocketType::Tcp)) return 0;
    sock.set_timeout(3000, 3000);
    // Head and body leave as two writes; without Nagle the body does not
    // wait for the server to acknowledge the head
    sock.set_tcp_nodelay(true);
    if (!sock.connect(host_.c_str(), port_)) return 0;
    if (!sock.send_all(buf, static_cast<size_t>(head_len))) return 0;
    if (!body.empty() && !sock.send_all(body.data(), body.size())) return 0;

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
    // The response head is collected in `buf` (the request is gone)
    size_t have = 0;
    while (status == 0) {
        if (have == sizeof(buf)) return 0;   // head larger than the buffer
        int n = sock.recv(buf + have, sizeof(buf) - have);
        if (n <= 0) return 0;
        have += static_cast<size_t>(n);
        const std::string_view head(buf, have);
        const size_t end = head.find("\r\n\r\n");
        if (end == std::string_view::npos) continue;
        const size_t sp = head.find(' ');
        if (sp == std::string_view::npos || sp > end) return 0;
        status = std::atoi(buf + sp + 1);
        if (status <= 0) return 0;
        chunked = is_chunked_transfer_encoding(head.substr(0, end));
        if (out_content_length) {
            const std::string_view cl = header_value(head.substr(0, end + 2), "content-length");
            if (!cl.empty()) *out_content_length = std::strtol(cl.data(), nullptr, 10);   // ends at the "\r"
        }
        // Body bytes that arrived with the head
        if (have > end + 4) feed(buf + end + 4, have - end - 4);
    }
    while (!done && !stop) {
        int n = sock.recv(buf, sizeof(buf));
        if (n <= 0) break;
        feed(buf, static_cast<size_t>(n));
    }
    // A chunked body ends before the connection does: wait for the server to
    // close it (Connection: close) so the close here is the passive one, and
    // the connection is not kept in TIME_WAIT on this node
    if (done) {
        while (sock.recv(buf, sizeof(buf)) > 0) {}
    }
    return status;
}

int HttpKubeClient::exchange(const char* method, std::string_view path, std::string_view body, BodyStart* start) {
    if (start) start->len = 0;
    return stream_request(method, path, body, [&](const char* data, size_t len) {
        if (start && start->len < sizeof(start->text)) {
            const size_t take = std::min(len, sizeof(start->text) - start->len);
            std::memcpy(start->text + start->len, data, take);
            start->len += take;
        }
        return true;
    });
}

bool HttpKubeClient::register_node(std::string_view node_json, bool& out_conflict) {
    out_conflict = false;
    const int status = exchange("POST", "/api/v1/nodes", node_json);
    if (status == 201) {
        return true;
    }
    if (status == 409) {
        out_conflict = true;
        return true;
    }
    // The kubelet retries every loop: one line per 10 s
    static uint64_t last_log_ms = 0;
    const uint64_t now = osal::System::get_uptime_ms();
    if (last_log_ms == 0 || now - last_log_ms >= 10000) {
        last_log_ms = now;
        std::cout << "[KUBELET] node registration failed: http " << status << std::endl;
    }
    return false;
}

bool HttpKubeClient::patch_node_status(const std::string& node_name, std::string_view patch_json) {
    PathBuf path;
    path.add("/api/v1/nodes/").add(node_name);
    BodyStart answer;
    const int status = exchange("PATCH", path.view(), patch_json, &answer);
    if (status != 200) {
        std::cout << "[KUBELET] node status patch failed: http " << status
                  << (answer.len ? " " : "") << std::string_view(answer.text, std::min<size_t>(answer.len, 120)) << std::endl;
    }
    return status == 200;
}

bool HttpKubeClient::renew_node_lease(const std::string& node_name, uint32_t duration_sec, std::string_view renew_time_iso8601) {
    PathBuf path;
    path.add("/apis/coordination.k8s.io/v1/namespaces/kube-node-lease/leases/").add(node_name);
    // renewTime is omitted while the node has no wall clock; the API server
    // stamps the renewal on arrival.
    char lease_json[384];
    const int n = snprintf(lease_json, sizeof(lease_json),
                           "{\"apiVersion\":\"coordination.k8s.io/v1\",\"kind\":\"Lease\","
                           "\"metadata\":{\"name\":\"%s\",\"namespace\":\"kube-node-lease\"},"
                           "\"spec\":{\"holderIdentity\":\"%s\",\"leaseDurationSeconds\":%u%s%.*s%s}}",
                           node_name.c_str(), node_name.c_str(), static_cast<unsigned>(duration_sec),
                           renew_time_iso8601.empty() ? "" : ",\"renewTime\":\"", static_cast<int>(renew_time_iso8601.size()),
                           renew_time_iso8601.data(), renew_time_iso8601.empty() ? "" : "\"");
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(lease_json)) return false;
    const std::string_view body(lease_json, static_cast<size_t>(n));

    const int status = exchange("PUT", path.view(), body);
    if (status == 200 || status == 201) {
        return true;
    }
    if (status == 404) {
        const int created = exchange("POST", "/apis/coordination.k8s.io/v1/namespaces/kube-node-lease/leases", body);
        return created == 200 || created == 201;
    }
    return false;
}

bool HttpKubeClient::list_pods(std::vector<models::KubePod>& out_pods) {
    std::string body;
    if (get("/api/v1/pods", body) != 200) {
        return false;
    }

    models::JsonValue doc;
    std::string err;
    if (!models::parse_json(body, doc, err) || !doc.is_object()) {
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

// One list item as it arrives: in storage the caller owns (an array on its
// stack) while it fits, on the heap for the rest of a larger item.
class ItemBuffer {
public:
    ItemBuffer(char* fixed, size_t capacity) : fixed_(fixed), cap_(capacity) {}
    void clear() {
        len_ = 0;
        spill_.clear();
        spilled_ = false;
    }
    void push_back(char c) {
        if (!spilled_) {
            if (len_ < cap_) {
                fixed_[len_++] = c;
                return;
            }
            spill_.assign(fixed_, len_);
            spilled_ = true;
        }
        spill_.push_back(c);
    }
    std::string_view view() const { return spilled_ ? std::string_view(spill_) : std::string_view(fixed_, len_); }
    std::string take() { return spilled_ ? std::move(spill_) : std::string(fixed_, len_); }

private:
    char* fixed_;
    size_t cap_;
    size_t len_{0};
    std::string spill_;
    bool spilled_{false};
};

// Splits a list response into its items as the bytes arrive: each element of
// the top-level "items" array is collected in `item` and handed over once
// complete.
class ListItemSplitter {
public:
    // `on_item` must outlive the splitter
    ListItemSplitter(ItemBuffer& item, mem::FunctionRef<void(ItemBuffer&)> on_item) : item_(item), on_item_(on_item) {
        item_.clear();
    }
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
                    if (depth_ == 1 && !in_items_) {
                        std::memcpy(last_key_, key_, key_len_);
                        last_key_len_ = key_len_;
                    }
                } else if (depth_ == 1 && key_len_ < sizeof(key_)) key_[key_len_++] = c;
                continue;
            }
            switch (c) {
                case '"':
                    in_string_ = true;
                    key_len_ = 0;
                    break;
                case '{':
                case '[':
                    if (c == '[' && depth_ == 1 && std::string_view(last_key_, last_key_len_) == "items") in_items_ = true;
                    ++depth_;
                    if (in_items_ && depth_ == 3 && c == '{') { item_.clear(); item_.push_back('{'); }
                    break;
                case '}':
                case ']':
                    --depth_;
                    if (depth_ == 0) complete_ = true;
                    if (in_items_ && depth_ == 2 && c == '}') {
                        on_item_(item_);
                        item_.clear();
                    } else if (in_items_ && depth_ == 1) {
                        in_items_ = false;
                        last_key_len_ = 0;
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
    ItemBuffer& item_;
    mem::FunctionRef<void(ItemBuffer&)> on_item_;
    bool complete_{false};
    char key_[16];
    char last_key_[16];
    size_t key_len_{0};
    size_t last_key_len_{0};
    int depth_{0};
    bool in_string_{false};
    bool escape_{false};
    bool in_items_{false};
};

// Reads the few fields the pod sync needs straight from a pod's JSON: no
// tree and no copies, the fields are views into the text. Strings are taken
// raw (names, uids and phases carry no escapes).
class PodFieldScanner {
public:
    explicit PodFieldScanner(std::string_view text) : p_(text.data()), end_(text.data() + text.size()) {}

    bool scan(PodListEntry& e) {
        return object([&](std::string_view key) {
            if (key == "metadata") {
                return object([&](std::string_view k) {
                    if (k == "name") return string(e.name);
                    if (k == "namespace") return string(e.namespace_name);
                    if (k == "uid") return string(e.uid);
                    if (k == "deletionTimestamp") {
                        std::string_view v;
                        if (!string(v)) return false;
                        e.deleting = !v.empty();
                        return true;
                    }
                    return skip();
                });
            }
            if (key == "spec") return object([&](std::string_view k) { return k == "nodeName" ? string(e.node_name) : skip(); });
            if (key == "status") return object([&](std::string_view k) { return k == "phase" ? string(e.phase) : skip(); });
            return skip();
        });
    }

private:
    void ws() {
        while (p_ < end_ && (*p_ == ' ' || *p_ == '\n' || *p_ == '\r' || *p_ == '\t')) ++p_;
    }
    // At a '"': the raw text up to the closing quote
    bool raw_string(std::string_view& out) {
        if (p_ >= end_ || *p_ != '"') return false;
        const char* start = ++p_;
        while (p_ < end_ && *p_ != '"') p_ += (*p_ == '\\') ? 2 : 1;
        if (p_ >= end_) return false;
        out = std::string_view(start, static_cast<size_t>(p_ - start));
        ++p_;
        return true;
    }
    // A string value; anything else (null) is skipped and leaves `out` empty
    bool string(std::string_view& out) {
        ws();
        return (p_ < end_ && *p_ == '"') ? raw_string(out) : skip();
    }
    bool skip() {
        ws();
        if (p_ >= end_) return false;
        std::string_view unused;
        if (*p_ == '"') return raw_string(unused);
        if (*p_ != '{' && *p_ != '[') {   // number, true, false, null
            while (p_ < end_ && *p_ != ',' && *p_ != '}' && *p_ != ']') ++p_;
            return true;
        }
        int depth = 0;
        while (p_ < end_) {
            if (*p_ == '"') {
                if (!raw_string(unused)) return false;
                continue;
            }
            if (*p_ == '{' || *p_ == '[') {
                ++depth;
            } else if (*p_ == '}' || *p_ == ']') {
                if (--depth == 0) {
                    ++p_;
                    return true;
                }
            }
            ++p_;
        }
        return false;
    }
    // An object, each member's value read by `member(key)`; any other value
    // is skipped
    template <typename F>
    bool object(F&& member) {
        ws();
        if (p_ >= end_) return false;
        if (*p_ != '{') return skip();
        ++p_;
        ws();
        if (p_ < end_ && *p_ == '}') {
            ++p_;
            return true;
        }
        for (;;) {
            ws();
            std::string_view key;
            if (!raw_string(key)) return false;
            ws();
            if (p_ >= end_ || *p_ != ':') return false;
            ++p_;
            if (!member(key)) return false;
            ws();
            if (p_ >= end_) return false;
            if (*p_ == ',') {
                ++p_;
                continue;
            }
            if (*p_ != '}') return false;
            ++p_;
            return true;
        }
    }

    const char* p_;
    const char* end_;
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

bool KubeApiClient::for_each_node_pod(mem::FunctionRef<void(const PodListEntry&)> visit) {
    std::vector<std::pair<models::KubePod, std::string>> pods;
    if (!list_pods_raw(pods)) return false;
    for (const auto& item : pods) {
        const models::KubePod& pod = item.first;
        PodListEntry e;
        e.name = pod.name;
        e.namespace_name = pod.namespace_name;
        e.uid = pod.uid;
        e.node_name = pod.node_name;
        e.phase = pod.phase;
        e.deleting = !pod.deletion_timestamp.empty();
        e.manifest = item.second;
        e.pod = &pod;
        visit(e);
    }
    return true;
}

int HttpKubeClient::list_items(const std::string& path, const std::function<void(std::string&&)>& on_item) {
    ItemBuffer item(nullptr, 0);
    auto hand_over = [&](ItemBuffer& done) { on_item(done.take()); };
    ListItemSplitter splitter(item, hand_over);
    const int status = stream_request("GET", path, std::string_view(), [&](const char* data, size_t len) {
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

bool HttpKubeClient::for_each_node_pod(mem::FunctionRef<void(const PodListEntry&)> visit) {
    PathBuf path;
    if (node_filter_.empty()) path.add("/api/v1/pods?inlineImages=omit");
    else path.add("/api/v1/pods?fieldSelector=spec.nodeName%3D").add(node_filter_).add("&inlineImages=omit");
    // A pod whose fields cannot be read makes the list unreadable: it must
    // not count as a pod gone
    bool unreadable = false;
    auto read_pod = [&](ItemBuffer& item) {
        if (unreadable) return;
        PodListEntry e;
        if (!PodFieldScanner(item.view()).scan(e) || e.name.empty()) {
            unreadable = true;
            return;
        }
        e.manifest = item.view();
        visit(e);
    };
    // A pod's JSON is collected on this stack (a few KiB; inline images are
    // left out of the list)
    char item_storage[3072];
    ItemBuffer item(item_storage, sizeof(item_storage));
    ListItemSplitter splitter(item, read_pod);
    const int status = stream_request("GET", path.view(), std::string_view(), [&](const char* data, size_t len) {
        splitter.feed(data, len);
        return !unreadable;
    });
    return status == 200 && splitter.complete() && !unreadable;
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
    PathBuf path;
    path.add("/api/v1/namespaces/").add(ns).add("/pods/").add(name).add("/status");
    const int status = exchange("PATCH", path.view(), status_patch_json);
    if (status == 200) {
        return true;
    }
    // Also try PUT if PATCH returns 405
    if (status == 405) {
        return exchange("PUT", path.view(), status_patch_json) == 200;
    }
    return false;
}


bool HttpKubeClient::create_event(const std::string& ns, const std::string& event_json) {
    PathBuf path;
    path.add("/api/v1/namespaces/").add(ns).add("/events");
    const int status = exchange("POST", path.view(), event_json);
    return status == 201 || status == 200;
}

bool HttpKubeClient::push_pod_logs(const std::string& ns, const std::string& name, const std::string& ndjson) {
    PathBuf path;
    path.add("/api/v1/namespaces/").add(ns).add("/pods/").add(name).add("/log");
    return exchange("POST", path.view(), ndjson) == 200;
}

int HttpKubeClient::get(const std::string& path, std::string& out_body) {
    out_body.clear();
    bool too_big = false;
    int status = stream_request("GET", path, std::string_view(), [&](const char* data, size_t len) {
        if (max_body_ && out_body.size() + len > max_body_) {
            too_big = true;
            return false;
        }
        out_body.append(data, len);
        return true;
    });
    if (too_big) {
        std::cout << "[KUBELET] GET " << path.substr(0, 80) << ": response over " << max_body_ << " bytes, dropped" << std::endl;
        status = 0;
        std::string().swap(out_body);
    }
    return status;
}

bool HttpKubeClient::delete_pod(const std::string& ns, const std::string& name) {
    PathBuf path;
    path.add("/api/v1/namespaces/").add(ns).add("/pods/").add(name).add("?gracePeriodSeconds=0");
    const int status = exchange("DELETE", path.view(), std::string_view());
    return status == 200 || status == 404;
}

bool HttpKubeClient::push_metrics(const std::string& node_name, std::string_view sample_json) {
    PathBuf path;
    path.add("/apis/metrics.k8s.io/v1beta1/nodes/").add(node_name);
    return exchange("POST", path.view(), sample_json) == 200;
}

} // namespace kube32::kubelet
