#pragma once

// Node HTTP endpoints for ESP-Mosaico expansion modules (served on the node's
// pod server and reachable through the API server's NodeProxyHandler at
// /api/v1/nodes/<node>/proxy/...):
//
//   GET  /modules          JSON status of left/right slots, Camera, Interaction inputs
//   GET  /camera/snapshot  Live JPEG snapshot (image/jpeg; optional ?quality=10..95)
//   POST /camera/preview   Captures one frame and blits RGB565 preview to the 480x480 AMOLED

#include "api/http_server.hpp"

#include <string>
#include <string_view>

namespace kube32::app {

bool is_node_mosaico_path(std::string_view path);
bool serve_node_mosaico(api::HttpConnection& conn, std::string_view path, const std::string& token);

}  // namespace kube32::app
