#include "kubelet/kube_api_client.hpp"
#include "osal/socket.hpp"
#include "models/kube_models.hpp"

#include <sstream>
#include <iostream>
#include <cstring>

namespace kube32::kubelet {

HttpKubeClient::HttpKubeClient(std::string host, uint16_t port)
    : host_(std::move(host)), port_(port) {}

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

    // Extract Body
    size_t header_end = raw_response.find("\r\n\r\n");
    if (header_end != std::string::npos) {
        resp.body = raw_response.substr(header_end + 4);
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
    return resp.status_code == 200;
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

} // namespace kube32::kubelet
