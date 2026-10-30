#include "app/egress_policy.hpp"
#include "models/kube_models.hpp"
#include "storage/store.hpp"
#include "osal/mutex.hpp"
#include "osal/system.hpp"

#include <atomic>
#include <map>

namespace kube32::app {

namespace {

bool none_egress(const net::NamespacePolicies& policies) {
    for (const auto& p : policies) {
        if (p.egress) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Control plane: the object store
// ---------------------------------------------------------------------------
class StoreEgressSource : public EgressPolicySource {
public:
    explicit StoreEgressSource(storage::Store& store)
        : store_(store), policies_dirty_(std::make_shared<std::atomic<bool>>(false)), pods_dirty_(std::make_shared<std::atomic<bool>>(false)) {
        std::shared_ptr<std::atomic<bool>> p = policies_dirty_;
        watches_.push_back(store_.watch("/registry/networkpolicies/", [p](const storage::WatchEvent&) { p->store(true); }));
        for (const char* prefix : {"/registry/pods/", "/registry/namespaces/"}) {
            std::shared_ptr<std::atomic<bool>> d = pods_dirty_;
            watches_.push_back(store_.watch(prefix, [d](const storage::WatchEvent&) { d->store(true); }));
        }
    }
    ~StoreEgressSource() override {
        for (uint32_t id : watches_) store_.unwatch(id);
    }

    bool scope(std::string_view pod, EgressScope& out, std::string&) override {
        const std::string ns(pod.substr(0, pod.find('/')));
        out.policies = policies_of(ns);
        out.selecting.clear();
        if (out.policies->empty() || none_egress(*out.policies)) return true;
        net::PeerPod self;
        peer(std::string(pod), self);   // a pod no longer stored has no labels: only empty selectors pick it
        out.selecting = net::select_egress_policies(*out.policies, self.labels);
        return true;
    }

    bool peer(const std::string& pod, net::PeerPod& out) override {
        {
            osal::LockGuard guard(mutex_);
            if (pods_dirty_->exchange(false)) pods_.clear();
            auto it = pods_.find(pod);
            if (it != pods_.end()) {
                out = it->second.pod;
                return it->second.ok;
            }
        }
        Cached c;
        std::string json, err;
        models::JsonValue doc;
        if (store_.get("/registry/pods/" + pod, json).ok() && models::parse_json(json, doc, err) && doc.is_object()) {
            const std::string ns = pod.substr(0, pod.find('/'));
            std::string ns_json;
            models::JsonValue ns_doc;
            const bool have_ns = store_.get("/registry/namespaces/" + ns, ns_json).ok() && models::parse_json(ns_json, ns_doc, err);
            c.pod = net::peer_pod_of(doc, have_ns ? &ns_doc : nullptr, ns);
            c.ok = true;
        }
        out = c.pod;
        const bool ok = c.ok;
        osal::LockGuard guard(mutex_);
        if (pods_.size() > 16) pods_.clear();
        pods_[pod] = std::move(c);
        return ok;
    }

private:
    std::shared_ptr<const net::NamespacePolicies> policies_of(const std::string& ns) {
        {
            osal::LockGuard guard(mutex_);
            if (policies_dirty_->exchange(false)) policies_.clear();
            auto it = policies_.find(ns);
            if (it != policies_.end()) return it->second;
        }
        std::vector<std::string> jsons;
        store_.list("/registry/networkpolicies/" + ns + "/", jsons);
        net::NamespacePolicies compiled;
        for (const auto& j : jsons) {
            models::JsonValue doc;
            std::string err;
            if (models::parse_json(j, doc, err) && doc.is_object()) compiled.push_back(net::compile_network_policy(doc));
        }
        auto shared = std::make_shared<const net::NamespacePolicies>(std::move(compiled));
        osal::LockGuard guard(mutex_);
        if (policies_.size() > 16) policies_.clear();
        policies_[ns] = shared;
        return shared;
    }

    struct Cached {
        bool ok{false};
        net::PeerPod pod;
    };
    storage::Store& store_;
    std::shared_ptr<std::atomic<bool>> policies_dirty_;
    std::shared_ptr<std::atomic<bool>> pods_dirty_;
    std::vector<uint32_t> watches_;
    osal::Mutex mutex_;
    std::map<std::string, std::shared_ptr<const net::NamespacePolicies>> policies_;
    std::map<std::string, Cached> pods_;
};

// ---------------------------------------------------------------------------
// Workers: the kubelet's view, target pods from the API server
// ---------------------------------------------------------------------------
class NodeEgressSource : public EgressPolicySource {
public:
    NodeEgressSource(std::shared_ptr<net::NodeNetworkPolicies> policies, std::function<int(const std::string&, std::string&)> get, uint32_t ttl)
        : policies_(std::move(policies)), get_(std::move(get)), ttl_ms_(ttl) {}

    bool scope(std::string_view pod, EgressScope& out, std::string& why) override {
        net::LabelMap labels;
        out.selecting.clear();
        if (!policies_->lookup(pod, labels, out.policies)) {
            why = "the network policies of namespace " + std::string(pod.substr(0, pod.find('/'))) + " are not loaded yet";
            return false;
        }
        out.selecting = net::select_egress_policies(*out.policies, labels);
        return true;
    }

    bool peer(const std::string& pod, net::PeerPod& out) override {
        const uint64_t now = osal::System::get_uptime_ms();
        {
            osal::LockGuard guard(mutex_);
            auto it = cache_.find(pod);
            if (it != cache_.end() && now - it->second.at_ms < ttl_ms_) {
                out = it->second.pod;
                return it->second.ok;
            }
        }
        // The pod as a one-item list: lists leave inline images out
        Cached c;
        c.at_ms = now;
        const size_t slash = pod.find('/');
        const std::string ns = pod.substr(0, slash);
        std::string body, err;
        int code = get_("/api/v1/namespaces/" + ns + "/pods?fieldSelector=metadata.name%3D" + pod.substr(slash + 1) + "&inlineImages=omit", body);
        models::JsonValue list;
        if (code == 200 && models::parse_json(body, list, err)) {
            const models::JsonValue* items = list.get("items");
            if (items && items->is_array() && !items->arr_val.empty()) {
                std::string().swap(body);
                models::JsonValue ns_doc;
                const bool have_ns = get_("/api/v1/namespaces/" + ns, body) == 200 && models::parse_json(body, ns_doc, err);
                c.pod = net::peer_pod_of(items->arr_val[0], have_ns ? &ns_doc : nullptr, ns);
                c.ok = true;
            }
        }
        out = c.pod;
        const bool ok = c.ok;
        osal::LockGuard guard(mutex_);
        if (cache_.size() > 8) cache_.clear();
        cache_[pod] = std::move(c);
        return ok;
    }

private:
    struct Cached {
        bool ok{false};
        uint64_t at_ms{0};
        net::PeerPod pod;
    };
    std::shared_ptr<net::NodeNetworkPolicies> policies_;
    std::function<int(const std::string&, std::string&)> get_;
    uint32_t ttl_ms_;
    osal::Mutex mutex_;
    std::map<std::string, Cached> cache_;
};

} // namespace

std::unique_ptr<EgressPolicySource> make_store_egress_source(storage::Store& store) {
    return std::make_unique<StoreEgressSource>(store);
}

std::unique_ptr<EgressPolicySource> make_node_egress_source(std::shared_ptr<net::NodeNetworkPolicies> policies,
                                                            std::function<int(const std::string&, std::string&)> api_get, uint32_t ttl_ms) {
    return std::make_unique<NodeEgressSource>(std::move(policies), std::move(api_get), ttl_ms);
}

} // namespace kube32::app
