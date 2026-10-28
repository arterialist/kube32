#include "kubelet/kube_api_client.hpp"
#include "osal/socket.hpp"
#include "models/kube_models.hpp"

#include <sstream>
#include <iostream>
#include <cstring>
#include <cctype>

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

HttpKubeClient::HttpResponse HttpKubeClient::send_http_request(const char* method, const std::string& path, const std::string& body) {
    HttpResponse resp;
    resp.status_code = 0;

    osal::Socket sock;
    if (!sock.create(osal::SocketType::Tcp)) {
        return resp;
    }

    sock.set_timeout(3000, 3000);

    if (!sock.connect(host_.c_str(), port_)) {
        return resp;
    }

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
    req += "Connection: close\r\n\r\n";
    req += body;

    if (!sock.send_all(req.data(), req.size())) {
        return resp;
    }

    std::string raw_response;
    char buf[4096];
    while (true) {
        int n = sock.recv(buf, sizeof(buf));
        if (n <= 0) break;
        raw_response.append(buf, n);
    }

    if (raw_response.empty()) {
        return resp;
    }

    // Parse HTTP Status
    size_t line_end = raw_response.find("\r\n");
    if (line_end != std::string::npos) {
        std::string status_line = raw_response.substr(0, line_end);
        // Expect: "HTTP/1.1 200 OK"
        size_t sp1 = status_line.find(' ');
        if (sp1 != std::string::npos) {
            size_t sp2 = status_line.find(' ', sp1 + 1);
            std::string code_str = (sp2 != std::string::npos) ?
                status_line.substr(sp1 + 1, sp2 - sp1 - 1) :
                status_line.substr(sp1 + 1);
            try {
                resp.status_code = std::stoi(code_str);
            } catch (...) {}
        }
    }

    // Extract Headers and Body
    size_t header_end = raw_response.find("\r\n\r\n");
    size_t header_sep_len = 4;
    if (header_end == std::string::npos) {
        header_end = raw_response.find("\n\n");
        header_sep_len = 2;
    }

    if (header_end != std::string::npos) {
        std::string_view headers_view(raw_response.data(), header_end);
        std::string_view raw_body(raw_response.data() + header_end + header_sep_len,
                                  raw_response.size() - (header_end + header_sep_len));

        if (is_chunked_transfer_encoding(headers_view)) {
            resp.body = decode_chunked_body(raw_body);
        } else {
            resp.body = std::string(raw_body);
        }
    }

    return resp;
}

bool HttpKubeClient::register_node(const std::string& node_json, bool& out_conflict) {
    out_conflict = false;
    HttpResponse resp = send_http_request("POST", "/api/v1/nodes", node_json);
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
    HttpResponse resp = send_http_request("PATCH", "/api/v1/nodes/" + node_name, patch_json);
    if (resp.status_code != 200) {
        std::cout << "[KUBELET] node status patch failed: http " << resp.status_code
                  << (resp.body.empty() ? std::string() : " " + resp.body.substr(0, 120)) << std::endl;
    }
    return resp.status_code == 200;
}

bool HttpKubeClient::renew_node_lease(const std::string& node_name, uint32_t duration_sec, const std::string& renew_time_iso8601) {
    std::string path = "/apis/coordination.k8s.io/v1/namespaces/kube-node-lease/leases/" + node_name;
    // renewTime is omitted while the node has no wall clock; the control plane
    // timestamps the renewal on arrival (NodeLifecycleController).
    std::string lease_json = "{\"apiVersion\":\"coordination.k8s.io/v1\",\"kind\":\"Lease\","
                             "\"metadata\":{\"name\":\"" + node_name + "\",\"namespace\":\"kube-node-lease\"},"
                             "\"spec\":{\"holderIdentity\":\"" + node_name + "\","
                             "\"leaseDurationSeconds\":" + std::to_string(duration_sec) +
                             (renew_time_iso8601.empty() ? std::string() : ",\"renewTime\":\"" + renew_time_iso8601 + "\"") + "}}";

    HttpResponse resp = send_http_request("PUT", path, lease_json);
    if (resp.status_code == 200 || resp.status_code == 201) {
        return true;
    }
    if (resp.status_code == 404) {
        HttpResponse post_resp = send_http_request("POST", "/apis/coordination.k8s.io/v1/namespaces/kube-node-lease/leases", lease_json);
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

bool HttpKubeClient::list_pods_raw(std::vector<std::pair<models::KubePod, std::string>>& out_pods_with_manifest) {
    std::string path = node_filter_.empty() ? std::string("/api/v1/pods") : ("/api/v1/pods?fieldSelector=spec.nodeName%3D" + node_filter_);
    HttpResponse resp = send_http_request("GET", path, "");
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
            std::string manifest_str = item.to_string();
            out_pods_with_manifest.emplace_back(models::parse_pod_manifest(manifest_str), std::move(manifest_str));
        }
    }
    return true;
}

bool HttpKubeClient::patch_pod_status(const std::string& ns, const std::string& name, const std::string& status_patch_json) {
    std::string path = "/api/v1/namespaces/" + ns + "/pods/" + name + "/status";
    HttpResponse resp = send_http_request("PATCH", path, status_patch_json);
    if (resp.status_code == 200) {
        return true;
    }
    // Also try PUT if PATCH returns 405
    if (resp.status_code == 405) {
        resp = send_http_request("PUT", path, status_patch_json);
        return resp.status_code == 200;
    }
    return false;
}


bool HttpKubeClient::create_event(const std::string& ns, const std::string& event_json) {
    HttpResponse resp = send_http_request("POST", "/api/v1/namespaces/" + ns + "/events", event_json);
    return resp.status_code == 201 || resp.status_code == 200;
}

bool HttpKubeClient::push_pod_logs(const std::string& ns, const std::string& name, const std::string& ndjson) {
    HttpResponse resp = send_http_request("POST", "/api/v1/namespaces/" + ns + "/pods/" + name + "/log", ndjson);
    return resp.status_code == 200;
}

bool HttpKubeClient::delete_pod(const std::string& ns, const std::string& name) {
    HttpResponse resp = send_http_request("DELETE", "/api/v1/namespaces/" + ns + "/pods/" + name + "?gracePeriodSeconds=0", "");
    return resp.status_code == 200 || resp.status_code == 404;
}

bool HttpKubeClient::push_metrics(const std::string& node_name, const std::string& sample_json) {
    HttpResponse resp = send_http_request("POST", "/apis/metrics.k8s.io/v1beta1/nodes/" + node_name, sample_json);
    return resp.status_code == 200;
}

} // namespace kube32::kubelet
