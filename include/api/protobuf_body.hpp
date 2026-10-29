#pragma once

// Request bodies in the Kubernetes protobuf encoding
// (application/vnd.kubernetes.protobuf) converted to the JSON the API server
// works with. Typed clients such as `kubectl create configmap`, `create
// secret` and `create namespace` send them and do not fall back to JSON.
// Supported kinds: v1 ConfigMap, Secret and Namespace.

#include <string>
#include <string_view>

namespace kube32::api {

// False with `out_error` for other kinds or a malformed message.
bool protobuf_body_to_json(std::string_view body, std::string& out_json, std::string& out_error);

} // namespace kube32::api
