#include "net/network_policy.hpp"
#include "models/kube_models.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace kube32::net {

namespace {

models::JsonValue jstr(const std::string& v) { return models::JsonValue{models::JsonType::String, false, 0, v, {}, {}}; }

const LabelMap::value_type* find_label(const LabelMap& labels, const std::string& key) {
    for (const auto& kv : labels) {
        if (kv.first == key) return &kv;
    }
    return nullptr;
}

bool contains(const std::vector<std::string>& values, const std::string& v) {
    return std::find(values.begin(), values.end(), v) != values.end();
}

// IANA_SVC_NAME, as a named port: 1-15 lowercase letters, digits and '-',
// at least one letter, no leading, trailing or doubled '-'
bool valid_port_name(const std::string& s) {
    if (s.empty() || s.size() > 15 || s.front() == '-' || s.back() == '-') return false;
    bool letter = false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c >= 'a' && c <= 'z') letter = true;
        else if (c == '-') { if (s[i - 1] == '-') return false; }
        else if (c < '0' || c > '9') return false;
    }
    return letter;
}

// IPv6 blocks pass validation as kube-apiserver accepts them; the cluster
// carries IPv4 only, so they match no connection
bool looks_like_ipv6_cidr(std::string_view text) {
    size_t slash = text.find('/');
    if (slash == std::string_view::npos || slash == 0 || text.find(':') == std::string_view::npos) return false;
    for (char c : text.substr(0, slash)) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == ':' || c == '.')) return false;
    }
    std::string_view bits = text.substr(slash + 1);
    if (bits.empty() || bits.size() > 3) return false;
    int v = 0;
    for (char c : bits) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    return v <= 128;
}

// The reason a check failed, from its parts: "<field>: <text>"
bool fail(std::string& why, std::initializer_list<std::string_view> parts) {
    why.clear();
    for (std::string_view p : parts) why.append(p.data(), p.size());
    return false;
}

// "<field>[<i>]"
std::string item(const std::string& field, size_t i) {
    char n[12];
    std::snprintf(n, sizeof n, "[%u]", static_cast<unsigned>(i));
    return field + n;
}

constexpr const char* kBadCidr = "\": must be a valid CIDR value, (e.g. 10.9.8.0/24 or 2001:db8::/64)";

bool validate_selector(const models::JsonValue* sel, const std::string& field, std::string& why) {
    if (!sel || sel->is_null()) return true;
    if (!sel->is_object()) return fail(why, {field, ": must be an object"});
    if (const models::JsonValue* ml = sel->get("matchLabels"); ml && !ml->is_null()) {
        if (!ml->is_object()) return fail(why, {field, ".matchLabels: must be an object"});
        for (const auto& kv : ml->obj_val) {
            if (!kv.second.is_string()) return fail(why, {field, ".matchLabels[", kv.first, "]: must be a string"});
        }
    }
    if (const models::JsonValue* me = sel->get("matchExpressions"); me && !me->is_null()) {
        if (!me->is_array()) return fail(why, {field, ".matchExpressions: must be a list"});
        for (size_t i = 0; i < me->arr_val.size(); ++i) {
            const models::JsonValue& e = me->arr_val[i];
            const std::string at = item(field + ".matchExpressions", i);
            const std::string op = e.get_string("operator", "");
            const models::JsonValue* values = e.get("values");
            const size_t n = (values && values->is_array()) ? values->arr_val.size() : 0;
            if (e.get_string("key", "").empty()) return fail(why, {at, ".key: Required value"});
            if (op == "In" || op == "NotIn") {
                if (n == 0) return fail(why, {at, ".values: Required value: must be specified when `operator` is 'In' or 'NotIn'"});
            } else if (op == "Exists" || op == "DoesNotExist") {
                if (n != 0) return fail(why, {at, ".values: Forbidden: may not be specified when `operator` is 'Exists' or 'DoesNotExist'"});
            } else {
                return fail(why, {at, ".operator: Invalid value: \"", op, "\": not a valid selector operator"});
            }
        }
    }
    return true;
}

bool validate_peers(const models::JsonValue* peers, const std::string& field, std::string& why) {
    if (!peers || peers->is_null()) return true;
    if (!peers->is_array()) return fail(why, {field, ": must be a list"});
    for (size_t i = 0; i < peers->arr_val.size(); ++i) {
        const models::JsonValue& p = peers->arr_val[i];
        const std::string at = item(field, i);
        const models::JsonValue* ps = p.get("podSelector");
        const models::JsonValue* ns = p.get("namespaceSelector");
        const models::JsonValue* ib = p.get("ipBlock");
        const bool has_sel = (ps && !ps->is_null()) || (ns && !ns->is_null());
        const bool has_ib = ib && !ib->is_null();
        if (!has_sel && !has_ib) return fail(why, {at, ": Required value: must specify a peer"});
        if (has_sel && has_ib) return fail(why, {at, ": Forbidden: may not specify both ipBlock and another peer"});
        if (!validate_selector(ps, at + ".podSelector", why) || !validate_selector(ns, at + ".namespaceSelector", why)) return false;
        if (!has_ib) continue;
        const std::string cidr = ib->get_string("cidr", "");
        uint32_t net = 0, mask = 0;
        const bool v4 = parse_cidr(cidr, net, mask);
        if (!v4 && !looks_like_ipv6_cidr(cidr)) return fail(why, {at, ".ipBlock.cidr: Invalid value: \"", cidr, kBadCidr});
        const models::JsonValue* ex = ib->get("except");
        if (!ex || ex->is_null()) continue;
        if (!ex->is_array()) return fail(why, {at, ".ipBlock.except: must be a list"});
        for (size_t k = 0; k < ex->arr_val.size(); ++k) {
            const std::string e = ex->arr_val[k].is_string() ? ex->arr_val[k].str_val : std::string();
            uint32_t enet = 0, emask = 0;
            if (parse_cidr(e, enet, emask)) {
                // A strict subset of cidr: a longer prefix inside the block
                if (!v4 || emask <= mask || (enet & mask) != net) {
                    return fail(why, {item(at + ".ipBlock.except", k), ": Invalid value: \"", e, "\": must be a strict subset of `cidr`"});
                }
            } else if (!looks_like_ipv6_cidr(e) || v4) {
                return fail(why, {item(at + ".ipBlock.except", k), ": Invalid value: \"", e, kBadCidr});
            }
        }
    }
    return true;
}

bool validate_ports(const models::JsonValue* ports, const std::string& field, std::string& why) {
    if (!ports || ports->is_null()) return true;
    if (!ports->is_array()) return fail(why, {field, ": must be a list"});
    for (size_t i = 0; i < ports->arr_val.size(); ++i) {
        const models::JsonValue& p = ports->arr_val[i];
        const std::string at = item(field, i);
        const std::string proto = p.get_string("protocol", "TCP");
        if (proto != "TCP" && proto != "UDP" && proto != "SCTP") {
            return fail(why, {at, ".protocol: Unsupported value: \"", proto, "\": supported values: \"SCTP\", \"TCP\", \"UDP\""});
        }
        const models::JsonValue* port = p.get("port");
        const models::JsonValue* end = p.get("endPort");
        const bool has_end = end && !end->is_null();
        int64_t number = 0;
        if (port && port->is_number()) {
            number = static_cast<int64_t>(port->num_val);
            if (number < 1 || number > 65535) {
                return fail(why, {at, ".port: Invalid value: ", std::to_string(number), ": must be between 1 and 65535, inclusive"});
            }
        } else if (port && port->is_string()) {
            if (!valid_port_name(port->str_val)) {
                return fail(why, {at, ".port: Invalid value: \"", port->str_val, "\": must be a valid port name (IANA_SVC_NAME)"});
            }
            if (has_end) return fail(why, {at, ".endPort: Invalid value: may not be specified when `port` is non-numeric"});
        } else if (port && !port->is_null()) {
            return fail(why, {at, ".port: must be a number or a port name"});
        } else if (has_end) {
            return fail(why, {at, ".endPort: Required value: may not be specified when `port` is not specified"});
        }
        if (has_end) {
            const int64_t e = end->is_number() ? static_cast<int64_t>(end->num_val) : -1;
            if (e < number || e > 65535) {
                return fail(why, {at, ".endPort: Invalid value: ", std::to_string(e), ": must be greater than or equal to `port`"});
            }
        }
    }
    return true;
}

bool validate_rules(const models::JsonValue* rules, const std::string& field, const char* peer_key, std::string& why) {
    if (!rules || rules->is_null()) return true;
    if (!rules->is_array()) return fail(why, {field, ": must be a list"});
    for (size_t i = 0; i < rules->arr_val.size(); ++i) {
        const models::JsonValue& r = rules->arr_val[i];
        const std::string at = item(field, i);
        if (!r.is_object()) return fail(why, {at, ": must be an object"});
        if (!validate_ports(r.get("ports"), at + ".ports", why)) return false;
        if (!validate_peers(r.get(peer_key), at + "." + peer_key, why)) return false;
    }
    return true;
}

LabelSelector compile_selector(const models::JsonValue* sel) {
    LabelSelector out;
    if (!sel || !sel->is_object()) return out;
    if (const models::JsonValue* ml = sel->get("matchLabels"); ml && ml->is_object()) {
        for (const auto& kv : ml->obj_val) {
            LabelRequirement r;
            r.key = kv.first;
            r.op = LabelRequirement::Op::In;
            r.values.push_back(kv.second.is_string() ? kv.second.str_val : std::string());
            out.requirements.push_back(std::move(r));
        }
    }
    if (const models::JsonValue* me = sel->get("matchExpressions"); me && me->is_array()) {
        for (const auto& e : me->arr_val) {
            LabelRequirement r;
            r.key = e.get_string("key", "");
            const std::string op = e.get_string("operator", "");
            r.op = op == "NotIn" ? LabelRequirement::Op::NotIn
                 : op == "Exists" ? LabelRequirement::Op::Exists
                 : op == "DoesNotExist" ? LabelRequirement::Op::DoesNotExist
                 : LabelRequirement::Op::In;
            if (const models::JsonValue* v = e.get("values"); v && v->is_array()) {
                for (const auto& s : v->arr_val) if (s.is_string()) r.values.push_back(s.str_val);
            }
            out.requirements.push_back(std::move(r));
        }
    }
    return out;
}

bool has_policy_type(const models::JsonValue* spec, const char* type) {
    const models::JsonValue* types = spec ? spec->get("policyTypes") : nullptr;
    if (!types || !types->is_array() || types->arr_val.empty()) {
        // Unset: Ingress always, Egress when there are egress rules
        if (std::string(type) == "Ingress") return true;
        const models::JsonValue* eg = spec ? spec->get("egress") : nullptr;
        return eg && eg->is_array() && !eg->arr_val.empty();
    }
    for (const auto& t : types->arr_val) {
        if (t.is_string() && t.str_val == type) return true;
    }
    return false;
}

bool peer_matches(const PolicyPeer& p, std::string_view policy_ns, const EgressTarget& t, const PeerPod*& pod, bool& asked,
                  mem::FunctionRef<const PeerPod*()> peer) {
    if (p.ip_block) {
        if (t.ip == 0 || (t.ip & p.mask) != p.net) return false;
        for (const auto& ex : p.except) {
            if ((t.ip & ex.second) == ex.first) return false;
        }
        return true;
    }
    if (t.pod_namespace.empty()) return false;   // selectors pick pods, not external hosts
    if (!asked) { pod = peer(); asked = true; }
    if (!pod) return false;
    if (p.has_namespace_selector) {
        if (!p.namespace_selector.matches(pod->namespace_labels)) return false;
    } else if (t.pod_namespace != policy_ns) {
        return false;
    }
    return !p.has_pod_selector || p.pod_selector.matches(pod->labels);
}

bool port_matches(const PolicyPort& p, const EgressTarget& t, const PeerPod*& pod, bool& asked, mem::FunctionRef<const PeerPod*()> peer) {
    if (!p.tcp) return false;
    if (!p.name.empty()) {
        if (t.pod_namespace.empty()) return false;   // named ports exist on pods only
        if (!asked) { pod = peer(); asked = true; }
        if (!pod) return false;
        for (const auto& np : pod->ports) {
            if (np.first == p.name && np.second == t.port) return true;
        }
        return false;
    }
    if (p.port == 0) return true;
    if (p.end_port) return t.port >= p.port && t.port <= p.end_port;
    return t.port == p.port;
}

} // namespace

bool LabelSelector::matches(const LabelMap& labels) const {
    for (const auto& r : requirements) {
        const LabelMap::value_type* l = find_label(labels, r.key);
        switch (r.op) {
            case LabelRequirement::Op::In:
                if (!l || !contains(r.values, l->second)) return false;
                break;
            case LabelRequirement::Op::NotIn:
                if (l && contains(r.values, l->second)) return false;
                break;
            case LabelRequirement::Op::Exists:
                if (!l) return false;
                break;
            case LabelRequirement::Op::DoesNotExist:
                if (l) return false;
                break;
        }
    }
    return true;
}

bool parse_ipv4(std::string_view text, uint32_t& out) {
    uint32_t addr = 0;
    int parts = 0;
    size_t i = 0;
    while (parts < 4) {
        if (i >= text.size() || text[i] < '0' || text[i] > '9') return false;
        uint32_t v = 0;
        size_t digits = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            v = v * 10 + static_cast<uint32_t>(text[i] - '0');
            if (++digits > 3 || v > 255) return false;
            ++i;
        }
        addr = (addr << 8) | v;
        if (++parts < 4) {
            if (i >= text.size() || text[i] != '.') return false;
            ++i;
        }
    }
    if (i != text.size()) return false;
    out = addr;
    return true;
}

bool parse_cidr(std::string_view text, uint32_t& net, uint32_t& mask) {
    size_t slash = text.find('/');
    if (slash == std::string_view::npos) return false;
    uint32_t addr = 0;
    if (!parse_ipv4(text.substr(0, slash), addr)) return false;
    std::string_view bits = text.substr(slash + 1);
    if (bits.empty() || bits.size() > 2) return false;
    uint32_t n = 0;
    for (char c : bits) {
        if (c < '0' || c > '9') return false;
        n = n * 10 + static_cast<uint32_t>(c - '0');
    }
    if (n > 32) return false;
    mask = n == 0 ? 0 : (0xFFFFFFFFu << (32 - n));
    net = addr & mask;
    return true;
}

std::string format_ipv4(uint32_t addr) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%u.%u.%u.%u", (addr >> 24) & 255, (addr >> 16) & 255, (addr >> 8) & 255, addr & 255);
    return buf;
}

LabelMap labels_of(const models::JsonValue& obj) {
    LabelMap out;
    const models::JsonValue* meta = obj.get("metadata");
    const models::JsonValue* labels = meta ? meta->get("labels") : nullptr;
    if (!labels || !labels->is_object()) return out;
    for (const auto& kv : labels->obj_val) {
        if (kv.second.is_string()) out.emplace_back(kv.first, kv.second.str_val);
    }
    return out;
}

PeerPod peer_pod_of(const models::JsonValue& pod, const models::JsonValue* ns, std::string_view ns_name) {
    PeerPod out;
    out.labels = labels_of(pod);
    if (ns) out.namespace_labels = labels_of(*ns);
    if (!find_label(out.namespace_labels, "kubernetes.io/metadata.name")) {
        out.namespace_labels.emplace_back("kubernetes.io/metadata.name", std::string(ns_name));
    }
    const models::JsonValue* spec = pod.get("spec");
    const models::JsonValue* containers = spec ? spec->get("containers") : nullptr;
    if (!containers || !containers->is_array()) return out;
    for (const auto& c : containers->arr_val) {
        const models::JsonValue* ports = c.get("ports");
        if (!ports || !ports->is_array()) continue;
        for (const auto& p : ports->arr_val) {
            const std::string name = p.get_string("name", "");
            const int64_t number = p.get_int("containerPort", 0);
            if (name.empty() || number <= 0 || number > 65535 || p.get_string("protocol", "TCP") != "TCP") continue;
            out.ports.emplace_back(name, static_cast<uint16_t>(number));
        }
    }
    return out;
}

bool validate_network_policy(const models::JsonValue& doc, std::string& why) {
    const models::JsonValue* spec = doc.get("spec");
    if (!spec || spec->is_null()) return true;
    if (!spec->is_object()) { why = "spec: must be an object"; return false; }
    if (!validate_selector(spec->get("podSelector"), "spec.podSelector", why)) return false;
    if (const models::JsonValue* types = spec->get("policyTypes"); types && !types->is_null()) {
        if (!types->is_array() || types->arr_val.size() > 2) { why = "spec.policyTypes: Invalid value: may not have more than 2 values"; return false; }
        for (size_t i = 0; i < types->arr_val.size(); ++i) {
            const std::string t = types->arr_val[i].is_string() ? types->arr_val[i].str_val : std::string();
            if (t != "Ingress" && t != "Egress") {
                why = "spec.policyTypes[" + std::to_string(i) + "]: Unsupported value: \"" + t + "\": supported values: \"Egress\", \"Ingress\"";
                return false;
            }
            if (i == 1 && types->arr_val[0].str_val == t) { why = "spec.policyTypes[1]: Duplicate value: \"" + t + "\""; return false; }
        }
    }
    return validate_rules(spec->get("ingress"), "spec.ingress", "from", why) &&
           validate_rules(spec->get("egress"), "spec.egress", "to", why);
}

void default_network_policy(models::JsonValue& doc) {
    models::JsonValue* spec = doc.get_mut("spec");
    if (!spec || !spec->is_object()) return;
    if (!spec->get("podSelector")) {
        models::JsonValue empty;
        empty.type = models::JsonType::Object;
        spec->set("podSelector", std::move(empty));
    }
    const models::JsonValue* types = spec->get("policyTypes");
    if (!types || !types->is_array() || types->arr_val.empty()) {
        models::JsonValue list;
        list.type = models::JsonType::Array;
        list.arr_val.push_back(jstr("Ingress"));
        if (has_policy_type(spec, "Egress")) list.arr_val.push_back(jstr("Egress"));
        spec->set("policyTypes", std::move(list));
    }
    for (const char* key : {"ingress", "egress"}) {
        models::JsonValue* rules = spec->get_mut(key);
        if (!rules || !rules->is_array()) continue;
        for (auto& r : rules->arr_val) {
            models::JsonValue* ports = r.get_mut("ports");
            if (!ports || !ports->is_array()) continue;
            for (auto& p : ports->arr_val) {
                if (p.is_object() && p.get_string("protocol", "").empty()) p.set("protocol", jstr("TCP"));
            }
        }
    }
}

NetworkPolicyRules compile_network_policy(const models::JsonValue& doc) {
    NetworkPolicyRules out;
    const models::JsonValue* meta = doc.get("metadata");
    out.name = meta ? meta->get_string("name", "") : "";
    std::string why;
    if (!validate_network_policy(doc, why)) {
        std::printf("[NETPOL] policy %s does not compile (%s): the pods of its namespace are denied every connection\n", out.name.c_str(), why.c_str());
        out.egress = true;
        return out;
    }
    const models::JsonValue* spec = doc.get("spec");
    if (!spec || !spec->is_object()) return out;
    out.pod_selector = compile_selector(spec->get("podSelector"));
    out.egress = has_policy_type(spec, "Egress");
    const models::JsonValue* rules = spec->get("egress");
    if (!out.egress || !rules || !rules->is_array()) return out;
    for (const auto& r : rules->arr_val) {
        EgressRule rule;
        if (const models::JsonValue* to = r.get("to"); to && to->is_array()) {
            for (const auto& p : to->arr_val) {
                PolicyPeer peer;
                if (const models::JsonValue* ib = p.get("ipBlock"); ib && ib->is_object()) {
                    peer.ip_block = true;
                    if (!parse_cidr(ib->get_string("cidr", ""), peer.net, peer.mask)) {
                        // An IPv6 block: 0.0.0.0/32, which no connection has
                        peer.net = 0;
                        peer.mask = 0xFFFFFFFFu;
                    }
                    if (const models::JsonValue* ex = ib->get("except"); ex && ex->is_array()) {
                        for (const auto& e : ex->arr_val) {
                            uint32_t enet = 0, emask = 0;
                            if (e.is_string() && parse_cidr(e.str_val, enet, emask)) peer.except.emplace_back(enet, emask);
                        }
                    }
                } else {
                    const models::JsonValue* ps = p.get("podSelector");
                    const models::JsonValue* ns = p.get("namespaceSelector");
                    peer.has_pod_selector = ps && ps->is_object();
                    peer.has_namespace_selector = ns && ns->is_object();
                    peer.pod_selector = compile_selector(ps);
                    peer.namespace_selector = compile_selector(ns);
                }
                rule.to.push_back(std::move(peer));
            }
        }
        if (const models::JsonValue* ports = r.get("ports"); ports && ports->is_array()) {
            for (const auto& p : ports->arr_val) {
                PolicyPort port;
                port.tcp = p.get_string("protocol", "TCP") == "TCP";
                if (const models::JsonValue* v = p.get("port"); v && v->is_number()) port.port = static_cast<uint16_t>(v->num_val);
                else if (v && v->is_string()) port.name = v->str_val;
                if (const models::JsonValue* e = p.get("endPort"); e && e->is_number()) port.end_port = static_cast<uint16_t>(e->num_val);
                rule.ports.push_back(std::move(port));
            }
        }
        out.egress_rules.push_back(std::move(rule));
    }
    return out;
}

std::vector<const NetworkPolicyRules*> select_egress_policies(const NamespacePolicies& policies, const LabelMap& labels) {
    std::vector<const NetworkPolicyRules*> out;
    for (const auto& p : policies) {
        if (p.egress && p.pod_selector.matches(labels)) out.push_back(&p);
    }
    return out;
}

bool egress_allowed(const std::vector<const NetworkPolicyRules*>& selected, std::string_view policy_ns, const EgressTarget& target,
                    mem::FunctionRef<const PeerPod*()> peer) {
    if (selected.empty()) return true;
    const PeerPod* pod = nullptr;
    bool asked = false;
    for (const NetworkPolicyRules* p : selected) {
        for (const auto& rule : p->egress_rules) {
            bool to = rule.to.empty();
            for (size_t i = 0; !to && i < rule.to.size(); ++i) to = peer_matches(rule.to[i], policy_ns, target, pod, asked, peer);
            if (!to) continue;
            bool port = rule.ports.empty();
            for (size_t i = 0; !port && i < rule.ports.size(); ++i) port = port_matches(rule.ports[i], target, pod, asked, peer);
            if (port) return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// NodeNetworkPolicies
// ---------------------------------------------------------------------------
bool NodeNetworkPolicies::pod_current(std::string_view pod, uint32_t labels_hash, uint32_t pass) {
    osal::LockGuard guard(mutex_);
    auto it = pods_.find(pod);
    if (it == pods_.end()) return false;
    it->second.pass = pass;
    return it->second.hash == labels_hash;
}

void NodeNetworkPolicies::set_pod(std::string_view pod, uint32_t labels_hash, LabelMap labels, uint32_t pass) {
    osal::LockGuard guard(mutex_);
    auto it = pods_.find(pod);
    if (it == pods_.end()) it = pods_.emplace(std::string(pod), Pod{}).first;
    it->second.hash = labels_hash;
    it->second.pass = pass;
    it->second.labels = std::move(labels);
}

// Called every pod sync: nothing is taken from the heap while every pod and
// namespace stays
void NodeNetworkPolicies::retain(uint32_t pass) {
    osal::LockGuard guard(mutex_);
    for (auto it = pods_.begin(); it != pods_.end();) {
        if (it->second.pass != pass) it = pods_.erase(it);
        else ++it;
    }
    // Pod keys are ordered: the first key not below "<ns>/" starts with it
    // when the namespace has a pod
    char prefix[80];
    for (auto it = policies_.begin(); it != policies_.end();) {
        const size_t n = it->first.size();
        bool has_pod = false;
        if (n + 1 < sizeof prefix) {
            std::memcpy(prefix, it->first.data(), n);
            prefix[n] = '/';
            const std::string_view p(prefix, n + 1);
            auto pod = pods_.lower_bound(p);
            has_pod = pod != pods_.end() && std::string_view(pod->first).substr(0, p.size()) == p;
        }
        if (!has_pod) it = policies_.erase(it);
        else ++it;
    }
}

std::vector<std::string> NodeNetworkPolicies::namespaces(bool unloaded_only) const {
    std::vector<std::string> out;
    std::string_view last;
    osal::LockGuard guard(mutex_);
    for (const auto& kv : pods_) {
        const std::string_view ns = std::string_view(kv.first).substr(0, kv.first.find('/'));
        if (ns == last) continue;   // ordered keys: a namespace's pods are adjacent
        last = ns;
        if (unloaded_only && policies_.find(ns) != policies_.end()) continue;
        out.emplace_back(ns);
    }
    return out;
}

void NodeNetworkPolicies::set_policies(const std::string& ns, NamespacePolicies policies) {
    auto shared = std::make_shared<const NamespacePolicies>(std::move(policies));
    osal::LockGuard guard(mutex_);
    policies_[ns] = std::move(shared);
}

bool NodeNetworkPolicies::lookup(std::string_view pod, LabelMap& labels, std::shared_ptr<const NamespacePolicies>& policies) const {
    std::string_view ns = pod.substr(0, pod.find('/'));
    osal::LockGuard guard(mutex_);
    auto p = pods_.find(pod);
    auto n = policies_.find(ns);
    if (p == pods_.end() || n == policies_.end()) return false;
    labels = p->second.labels;
    policies = n->second;
    return true;
}

size_t NodeNetworkPolicies::policy_count() const {
    osal::LockGuard guard(mutex_);
    size_t n = 0;
    for (const auto& kv : policies_) n += kv.second->size();
    return n;
}

} // namespace kube32::net
