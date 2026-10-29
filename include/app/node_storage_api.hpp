#pragma once

// Files on the node's storage root over HTTP, so images and data reach a
// node without taking its card out. Served by the node's kubelet endpoint
// (the pod HTTP server) and reached through the API server's node proxy:
//
//   /api/v1/nodes/<node>/proxy/storage/<path>  ->  <node>:<kubelet port>/storage/<path>
//
// <path> is relative to <storage root>/kube32 (hal/node_storage.hpp):
//   GET    /storage/<dir>   {"path":"<dir>","entries":[{"name":"x","size":12},{"name":"d","dir":true}]}
//   GET    /storage/<file>  the file (application/octet-stream, Content-Length)
//   PUT    /storage/<file>  the body (Content-Length) is written to <file>.part
//                           a buffer at a time, then renamed over <file>;
//                           missing directories are made. 201 new, 200 replaced.
//   DELETE /storage/<path>  removes a file or an empty directory
// Path components are [A-Za-z0-9._-], no "." or "..", at most 96 bytes in
// all. With a token set, requests must carry "Authorization: Bearer <token>"
// (the API server forwards its own).

#include "api/http_server.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace kube32::app {

// "/storage" or "/storage/..."
bool is_node_storage_path(std::string_view path);

// Answers one request for `path` ("/storage/...") on `conn`; its body is
// streamed (see HttpHandler::streams_body). Uploads larger than
// `max_upload_bytes` get 413.
bool serve_node_storage(api::HttpConnection& conn, std::string_view path, const std::string& token, size_t max_upload_bytes);

// "Authorization: Bearer <token>" check, constant time in the token's length.
// An empty token accepts every request.
bool bearer_token_matches(std::string_view authorization_header, const std::string& token);

} // namespace kube32::app
