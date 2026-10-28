#include "api/metrics_registry.hpp"
#include "models/kube_models.hpp"
#include "osal/system.hpp"

namespace kube32::api {

MetricsRegistry::MetricsRegistry(uint32_t stale_after_ms, size_t max_pods)
    : stale_after_ms_(stale_after_ms), max_pods_(max_pods == 0 ? 1 : max_pods) {}

bool MetricsRegistry::fresh(uint64_t received_uptime_ms, uint64_t now) const {
    return now - received_uptime_ms <= stale_after_ms_;
}

bool MetricsRegistry::ingest(const std::string& json, std::string& err) {
    models::JsonValue doc;
    if (!models::parse_json(json, doc, err) || !doc.is_object()) return false;
    std::string node = doc.get_string("node", "");
    if (node.empty()) { err = "node is required"; return false; }
    uint64_t now = osal::System::get_uptime_ms();
    uint64_t window = static_cast<uint64_t>(doc.get_int("window_ms", 0));
    uint64_t ts = 0;
    std::string ts_str = doc.get_string("timestamp", "");
    if (!ts_str.empty()) models::parse_rfc3339(ts_str, ts);

    NodeUsageSample ns;
    ns.node = node;
    ns.window_ms = window;
    ns.sample_unix_ms = ts;
    ns.received_uptime_ms = now;
    if (const auto* usage = doc.get("usage")) {
        if (usage->get("cpu_nanocores")) { ns.has_cpu = true; ns.cpu_nanocores = static_cast<uint64_t>(usage->get_int("cpu_nanocores", 0)); }
        if (usage->get("memory_bytes")) { ns.has_memory = true; ns.memory_bytes = static_cast<uint64_t>(usage->get_int("memory_bytes", 0)); }
    }

    osal::LockGuard guard(mutex_);
    nodes_[node] = ns;
    if (const auto* pods = doc.get("pods")) {
        if (pods->is_array()) {
            for (const auto& p : pods->arr_val) {
                if (!p.is_object()) continue;
                PodUsageSample ps;
                ps.namespace_name = p.get_string("namespace", "default");
                ps.name = p.get_string("name", "");
                if (ps.name.empty()) continue;
                ps.node = node;
                ps.window_ms = window;
                ps.sample_unix_ms = ts;
                ps.received_uptime_ms = now;
                if (const auto* cs = p.get("containers")) {
                    if (cs->is_array()) {
                        for (const auto& c : cs->arr_val) {
                            ContainerUsage cu;
                            cu.name = c.get_string("name", ps.name);
                            if (c.get("cpu_nanocores")) { cu.has_cpu = true; cu.cpu_nanocores = static_cast<uint64_t>(c.get_int("cpu_nanocores", 0)); }
                            if (c.get("memory_bytes")) { cu.has_memory = true; cu.memory_bytes = static_cast<uint64_t>(c.get_int("memory_bytes", 0)); }
                            if (c.get("psram_bytes")) { cu.has_psram = true; cu.psram_bytes = static_cast<uint64_t>(c.get_int("psram_bytes", 0)); }
                            ps.containers.push_back(std::move(cu));
                        }
                    }
                }
                std::string key = ps.namespace_name + "/" + ps.name;
                if (pods_.find(key) == pods_.end() && pods_.size() >= max_pods_) {
                    // Evict the stalest pod sample
                    auto victim = pods_.begin();
                    for (auto it = pods_.begin(); it != pods_.end(); ++it)
                        if (it->second.received_uptime_ms < victim->second.received_uptime_ms) victim = it;
                    pods_.erase(victim);
                }
                pods_[key] = std::move(ps);
            }
        }
    }
    return true;
}

bool MetricsRegistry::node(const std::string& name, NodeUsageSample& out) const {
    osal::LockGuard guard(mutex_);
    auto it = nodes_.find(name);
    if (it == nodes_.end() || !fresh(it->second.received_uptime_ms, osal::System::get_uptime_ms())) return false;
    out = it->second;
    return true;
}

std::vector<NodeUsageSample> MetricsRegistry::nodes() const {
    osal::LockGuard guard(mutex_);
    uint64_t now = osal::System::get_uptime_ms();
    std::vector<NodeUsageSample> out;
    for (const auto& kv : nodes_) if (fresh(kv.second.received_uptime_ms, now)) out.push_back(kv.second);
    return out;
}

bool MetricsRegistry::pod(const std::string& ns, const std::string& name, PodUsageSample& out) const {
    osal::LockGuard guard(mutex_);
    auto it = pods_.find(ns + "/" + name);
    if (it == pods_.end() || !fresh(it->second.received_uptime_ms, osal::System::get_uptime_ms())) return false;
    out = it->second;
    return true;
}

std::vector<PodUsageSample> MetricsRegistry::pods(const std::string& ns) const {
    osal::LockGuard guard(mutex_);
    uint64_t now = osal::System::get_uptime_ms();
    std::vector<PodUsageSample> out;
    for (const auto& kv : pods_) {
        if (!fresh(kv.second.received_uptime_ms, now)) continue;
        if (!ns.empty() && kv.second.namespace_name != ns) continue;
        out.push_back(kv.second);
    }
    return out;
}

void MetricsRegistry::forget_pod(const std::string& ns, const std::string& name) {
    osal::LockGuard guard(mutex_);
    pods_.erase(ns + "/" + name);
}

std::string MetricsRegistry::quantity_cpu(uint64_t nanocores) {
    return std::to_string(nanocores) + "n";
}

std::string MetricsRegistry::quantity_memory(uint64_t bytes) {
    if (bytes != 0 && bytes % 1024 == 0) return std::to_string(bytes / 1024) + "Ki";
    return std::to_string(bytes);
}

static void append_usage(std::string& out, bool has_cpu, uint64_t cpu, bool has_mem, uint64_t mem, bool has_psram = false, uint64_t psram = 0) {
    out += "\"usage\":{";
    bool first = true;
    if (has_cpu) { out += "\"cpu\":\"" + MetricsRegistry::quantity_cpu(cpu) + "\""; first = false; }
    if (has_mem) { if (!first) out += ","; out += "\"memory\":\"" + MetricsRegistry::quantity_memory(mem) + "\""; first = false; }
    // Extra resource name in the usage map: kubectl top ignores it, k9s and
    // the ingress telemetry can read it, and "memory" keeps the meaning of the
    // pod's memory request (internal SRAM).
    if (has_psram) { if (!first) out += ","; out += "\"esp32.io/psram\":\"" + MetricsRegistry::quantity_memory(psram) + "\""; }
    out += "}";
}

static std::string window_field(uint64_t window_ms) {
    char buf[32];
    snprintf(buf, sizeof(buf), "\"window\":\"%llu.%03us\"", static_cast<unsigned long long>(window_ms / 1000ULL),
             static_cast<unsigned>(window_ms % 1000ULL));
    return buf;
}

std::string MetricsRegistry::node_metrics_json(const NodeUsageSample& s) {
    std::string out = "{\"kind\":\"NodeMetrics\",\"apiVersion\":\"metrics.k8s.io/v1beta1\",\"metadata\":{\"name\":\"" + s.node + "\"";
    if (s.sample_unix_ms) out += ",\"creationTimestamp\":\"" + models::format_rfc3339(s.sample_unix_ms) + "\"";
    out += "},";
    if (s.sample_unix_ms) out += "\"timestamp\":\"" + models::format_rfc3339(s.sample_unix_ms) + "\",";
    out += window_field(s.window_ms) + ",";
    append_usage(out, s.has_cpu, s.cpu_nanocores, s.has_memory, s.memory_bytes);
    out += "}";
    return out;
}

std::string MetricsRegistry::pod_metrics_json(const PodUsageSample& s) {
    std::string out = "{\"kind\":\"PodMetrics\",\"apiVersion\":\"metrics.k8s.io/v1beta1\",\"metadata\":{\"name\":\"" + s.name +
                      "\",\"namespace\":\"" + s.namespace_name + "\"";
    if (s.sample_unix_ms) out += ",\"creationTimestamp\":\"" + models::format_rfc3339(s.sample_unix_ms) + "\"";
    out += "},";
    if (s.sample_unix_ms) out += "\"timestamp\":\"" + models::format_rfc3339(s.sample_unix_ms) + "\",";
    out += window_field(s.window_ms) + ",\"containers\":[";
    for (size_t i = 0; i < s.containers.size(); ++i) {
        if (i) out += ",";
        out += "{\"name\":\"" + s.containers[i].name + "\",";
        append_usage(out, s.containers[i].has_cpu, s.containers[i].cpu_nanocores, s.containers[i].has_memory, s.containers[i].memory_bytes,
                     s.containers[i].has_psram, s.containers[i].psram_bytes);
        out += "}";
    }
    out += "]}";
    return out;
}

} // namespace kube32::api
