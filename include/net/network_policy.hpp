#pragma once

// NetworkPolicy (networking.k8s.io/v1) egress rules in a compact form, and
// their evaluation for one connection of a pod (k32_http_call).
//
// A pod is restricted when at least one NetworkPolicy of its namespace
// selects it (spec.podSelector) and lists Egress in spec.policyTypes. It may
// then open only the connections that an egress rule of one of those
// policies allows. A rule allows a connection when one of its peers (`to`;
// none: any destination) and one of its ports (`ports`; none: any port)
// match:
//   ipBlock {cidr, except}   the destination address lies in cidr and in
//                            none of the excepted blocks
//   podSelector              in-cluster calls: pods of the policy's
//                            namespace whose labels match
//   namespaceSelector        in-cluster calls: pods of the namespaces whose
//                            labels match (with podSelector: matching pods
//                            of those namespaces)
//   port, endPort, protocol  TCP only; a named port matches the target
//                            pod's containerPort of that name
// Ingress rules are stored by the API server and not evaluated.

#include "mem/function_ref.hpp"
#include "osal/mutex.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kube32::models { struct JsonValue; }

namespace kube32::net {

using LabelMap = std::vector<std::pair<std::string, std::string>>;

struct LabelRequirement {
    enum class Op : uint8_t { In, NotIn, Exists, DoesNotExist };
    std::string key;
    Op op{Op::In};
    std::vector<std::string> values;
};

// metav1.LabelSelector: matchLabels and matchExpressions, all of which must
// hold. An empty selector selects everything.
struct LabelSelector {
    std::vector<LabelRequirement> requirements;
    bool matches(const LabelMap& labels) const;
};

struct PolicyPeer {
    bool ip_block{false};
    uint32_t net{0};        // host byte order
    uint32_t mask{0};
    std::vector<std::pair<uint32_t, uint32_t>> except;   // (net, mask)
    bool has_pod_selector{false};
    bool has_namespace_selector{false};
    LabelSelector pod_selector;
    LabelSelector namespace_selector;
};

struct PolicyPort {
    bool tcp{true};
    uint16_t port{0};       // 0 and no name: every port
    uint16_t end_port{0};   // 0: `port` alone
    std::string name;       // a containerPort name
};

struct EgressRule {
    std::vector<PolicyPeer> to;
    std::vector<PolicyPort> ports;
};

struct NetworkPolicyRules {
    std::string name;
    LabelSelector pod_selector;
    bool egress{false};     // spec.policyTypes holds Egress
    std::vector<EgressRule> egress_rules;
};
using NamespacePolicies = std::vector<NetworkPolicyRules>;

// Checks a NetworkPolicy as kube-apiserver validates it (selectors, CIDRs,
// ports, policyTypes). False with the field and the reason in `why`.
bool validate_network_policy(const models::JsonValue& doc, std::string& why);
// The defaults kube-apiserver applies: spec.policyTypes from the rules
// present (Ingress, and Egress when there are egress rules), protocol TCP
// on every port.
void default_network_policy(models::JsonValue& doc);
// Compiles a stored NetworkPolicy. One that does not compile selects every
// pod of its namespace for egress with no rule: those pods are denied every
// connection rather than allowed all.
NetworkPolicyRules compile_network_policy(const models::JsonValue& doc);

// metadata.labels of an object (string values only)
LabelMap labels_of(const models::JsonValue& obj);

bool parse_ipv4(std::string_view text, uint32_t& out);   // dotted quad -> host byte order
bool parse_cidr(std::string_view text, uint32_t& net, uint32_t& mask);
std::string format_ipv4(uint32_t addr);

// The pod behind an in-cluster call, as policy peers see it
struct PeerPod {
    LabelMap labels;
    LabelMap namespace_labels;
    std::vector<std::pair<std::string, uint16_t>> ports;   // TCP containerPort names
};

// A pod as policy peers see it, from its JSON and its namespace's (nullptr:
// none). The namespace carries the label kubernetes.io/metadata.name=<name>
// as every namespace does in Kubernetes.
PeerPod peer_pod_of(const models::JsonValue& pod, const models::JsonValue* ns, std::string_view ns_name);

struct EgressTarget {
    uint32_t ip{0};                   // host byte order; 0 when unknown
    uint16_t port{0};
    std::string_view pod_namespace;   // in-cluster calls: the target pod's namespace; empty for external hosts
};

// The egress policies of `policies` that select a pod with `labels`
std::vector<const NetworkPolicyRules*> select_egress_policies(const NamespacePolicies& policies, const LabelMap& labels);
// Whether the rules of `selected` (policies of namespace `policy_ns`) allow
// the connection. `peer` returns the target pod; it is asked at most once,
// and only when a pod or namespace selector or a named port is compared
// (nullptr: not known, nothing of the kind matches).
bool egress_allowed(const std::vector<const NetworkPolicyRules*>& selected, std::string_view policy_ns, const EgressTarget& target,
                    mem::FunctionRef<const PeerPod*()> peer);

// The network policies a worker enforces: those of the namespaces its pods
// run in, and the labels of those pods. The kubelet keeps it current from
// its pod sync; the pods' calls read it.
class NodeNetworkPolicies {
public:
    // Kubelet side. pod_current: whether `pod` ("<ns>/<name>") is known with
    // labels whose text hashes to `labels_hash`; marks it listed in `pass`.
    bool pod_current(std::string_view pod, uint32_t labels_hash, uint32_t pass);
    void set_pod(std::string_view pod, uint32_t labels_hash, LabelMap labels, uint32_t pass);
    // Forgets the pods not listed in `pass` and the policies of namespaces
    // left without pods
    void retain(uint32_t pass);
    // Namespaces of the known pods (only those whose policies are not loaded)
    std::vector<std::string> namespaces(bool unloaded_only) const;
    void set_policies(const std::string& ns, NamespacePolicies policies);

    // Call side: the labels of `pod` and the policies of its namespace.
    // False when the pod is unknown or its namespace's policies are not
    // loaded yet.
    bool lookup(std::string_view pod, LabelMap& labels, std::shared_ptr<const NamespacePolicies>& policies) const;
    size_t policy_count() const;

private:
    struct Pod {
        uint32_t hash{0};
        uint32_t pass{0};
        LabelMap labels;
    };
    mutable osal::Mutex mutex_;
    std::map<std::string, Pod, std::less<>> pods_;
    std::map<std::string, std::shared_ptr<const NamespacePolicies>, std::less<>> policies_;
};

} // namespace kube32::net
