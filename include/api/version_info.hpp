#pragma once

// Build identity of this binary, as served by GET /version and used for the
// kubernetes.io/arch node label.

#include <string>

namespace kube32::api {

// Kubernetes API level this server implements (version.Info.gitVersion).
constexpr const char* KUBE32_GIT_VERSION = "v1.33.0-kube32";

struct BuildInfo {
    const char* git_commit;      // full commit hash, "" when built outside a git checkout
    const char* git_tree_state;  // "clean", "dirty", or "" when unknown
    const char* build_date;      // RFC 3339 UTC time the build info was generated
    std::string compiler;        // compiler name and version, e.g. "gcc 14.2.0"
    const char* os;              // "darwin", "linux" or "freertos"
    const char* arch;            // "arm64", "amd64" or "xtensa"
};

const BuildInfo& build_info();

// JSON body of GET /version (k8s.io/apimachinery version.Info).
std::string version_json();

} // namespace kube32::api
