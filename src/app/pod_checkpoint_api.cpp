#include "app/pod_checkpoint_api.hpp"
#include "app/node_storage_api.hpp"
#include "api/selectors.hpp"
#include "osal/sha256.hpp"
#include "osal/system.hpp"
#include "runtime/app_pod.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace kube32::app {

namespace {

std::string json_string(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        if (static_cast<unsigned char>(c) >= 0x20) out.push_back(c);
    }
    return out + "\"";
}

bool reply(api::HttpConnection& conn, int status, const char* reason, const std::string& message) {
    return conn.send_simple(status, "application/json", "{\"reason\":\"" + std::string(reason) + "\",\"message\":" + json_string(message) + "}");
}

bool reply_blockers(api::HttpConnection& conn, int status, const char* reason, const std::vector<std::string>& blockers) {
    std::string body = "{\"reason\":\"" + std::string(reason) + "\",\"blockers\":[";
    for (size_t i = 0; i < blockers.size(); ++i) body += (i ? "," : "") + json_string(blockers[i]);
    return conn.send_simple(status, "application/json", body + "]}");
}

std::string hex(const uint8_t* d) {
    std::array<uint8_t, 32> a;
    std::memcpy(a.data(), d, 32);
    return osal::Sha256::hex(a);
}

} // namespace

runtime::SnapshotStream checkpoint_data_stream(kubelet::KubeApiClient& api, const std::string& ns, const std::string& name) {
    const std::string path = "/apis/kube32.io/v1/namespaces/" + ns + "/checkpoints/" + name + "/data";
    return [&api, path, name](mem::FunctionRef<bool(const uint8_t*, size_t)> feed, std::string& why) {
        char head[160];   // the start of the body, for an error's message
        size_t head_len = 0;
        const int status = api.stream_get(path, [&](const char* d, size_t n) {
            const size_t take = std::min(n, sizeof(head) - head_len);
            std::memcpy(head + head_len, d, take);
            head_len += take;
            return feed(reinterpret_cast<const uint8_t*>(d), n);
        });
        if (status == 200) return true;
        // "checkpoint <name>: http 404: <the server's message>"
        const std::string_view body(head, head_len);
        size_t m = body.find("\"message\":\"");
        m = m == std::string_view::npos ? head_len : m + 11;
        const size_t end = std::min(body.find('"', m), head_len);
        char line[224];
        if (status == 0) snprintf(line, sizeof(line), "checkpoint %s: the control plane did not answer", name.c_str());
        else snprintf(line, sizeof(line), "checkpoint %s: http %d%s%.*s", name.c_str(), status, end > m ? ": " : "", (int)(end - m), head + m);
        why = line;
        return false;
    };
}

bool is_pod_checkpoint_path(std::string_view path) {
    return path.rfind("/checkpoint/", 0) == 0 || path.rfind("/thaw/", 0) == 0;
}

bool serve_pod_checkpoint(api::HttpConnection& conn, std::string_view path, const std::string& token) {
    const api::HttpRequest& req = conn.request();
    if (!bearer_token_matches(req.authorization, token)) {
        conn.set_close();
        return reply(conn, 401, "Unauthorized", "Unauthorized");
    }
    const bool thaw = path.rfind("/thaw/", 0) == 0;
    path.remove_prefix(thaw ? 6 : 12);
    // <namespace>/<pod>/<container>
    const size_t a = path.find('/');
    const size_t b = a == std::string_view::npos ? a : path.find('/', a + 1);
    if (a == 0 || b == std::string_view::npos || b == a + 1 || b + 1 >= path.size() || path.find('/', b + 1) != std::string_view::npos) {
        return reply(conn, 404, "NotFound", "want /<namespace>/<pod>/<container>");
    }
    if (req.method != api::HttpMethod::Post) return reply(conn, 405, "MethodNotAllowed", "POST only");

    runtime::AppPodHost& host = runtime::AppPodHost::instance();
    const std::string pod_id(path.substr(0, b));
    std::string container;
    if (!host.container_name(pod_id, container) || container != path.substr(b + 1)) {
        return reply(conn, 404, "NotFound", "no such pod and container on this node");
    }
    if (thaw) {
        if (!host.thaw(pod_id)) return reply(conn, 404, "NotFound", "the pod is not frozen");
        return conn.send_simple(200, "application/json", "{\"thawed\":true}");
    }

    std::string leave = "running", timeout_text;
    api::query_param(req.query_string, "leave", leave);
    const bool stay_frozen = leave == "frozen";
    if (!stay_frozen && leave != "running") return reply(conn, 400, "BadRequest", "leave is running or frozen");
    uint32_t timeout_ms = 10000;
    if (api::query_param(req.query_string, "timeoutMs", timeout_text)) {
        timeout_ms = static_cast<uint32_t>(std::strtoul(timeout_text.c_str(), nullptr, 10));
        if (timeout_ms == 0 || timeout_ms > 60000) return reply(conn, 400, "BadRequest", "timeoutMs is 1 to 60000");
    }

    const std::vector<std::string> blockers = host.migration_blockers(pod_id);
    if (!blockers.empty()) return reply_blockers(conn, 409, "NotMovable", blockers);
    const uint64_t t0 = osal::System::get_uptime_ms();
    std::string why;
    const int rc = host.freeze(pod_id, timeout_ms, why);
    if (rc == -2) return reply(conn, 404, "NotFound", "the pod is not a request pod");
    if (rc == -1) return reply_blockers(conn, 504, "Timeout", {why});

    // The snapshot is read twice: once for its length and digest, which the
    // head carries, then into the socket. A frozen pod gives the same bytes.
    runtime::SnapshotSummary sum, sent;
    bool ok = host.snapshot(pod_id, [](const uint8_t*, size_t) { return true; }, sum, why);
    if (ok) {
        char headers[256];
        snprintf(headers, sizeof(headers), "X-Kube32-Image-Sha256: %s\r\nX-Kube32-Snapshot-Sha256: %s\r\nX-Kube32-Pod-Time-Ms: %llu\r\n",
                 hex(sum.image_sha).c_str(), hex(sum.digest).c_str(), static_cast<unsigned long long>(sum.pod_time_ms));
        conn.set_close();
        ok = conn.begin_fixed(200, "application/octet-stream", sum.bytes, headers) &&
             host.snapshot(pod_id, [&](const uint8_t* d, size_t n) { return conn.send_raw(d, n); }, sent, why) &&
             sent.bytes == sum.bytes && std::memcmp(sent.digest, sum.digest, 32) == 0;
    } else {
        reply(conn, 500, "InternalError", why);
    }
    if (!stay_frozen) host.thaw(pod_id);
    printf("[POD] %s: checkpoint of %u bytes in %u ms%s%s\n", pod_id.c_str(), (unsigned)sum.bytes, (unsigned)(osal::System::get_uptime_ms() - t0),
           stay_frozen ? ", left frozen" : "", ok ? "" : " (not sent whole)");
    return true;
}

} // namespace kube32::app
