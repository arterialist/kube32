#pragma once

// The node's blobs (storage/blob_store.hpp) over HTTP. Served by the node's
// kubelet endpoint and reached through the API server's node proxy:
//
//   /api/v1/nodes/<node>/proxy/blobs[/<name>]  ->  <node>:<kubelet port>/blobs[/<name>]
//
//   GET    /blobs          {"stores":[{"medium":"flash","location":"ota_1 (passive app slot)",
//                           "available":true,"capacity":3342336,"free":3276800,
//                           "blobs":[{"name":"w","size":1234,"crc32":"0x89abcdef"}]},
//                           {"medium":"sd",...}]}
//   GET    /blobs/<name>   the blob's bytes (application/octet-stream)
//   PUT    /blobs/<name>   writes the body as blob <name>, replacing one of that
//                          name once the whole body is in: 201 new, 200 replaced
//   DELETE /blobs/<name>   removes the blob
//
// Query parameters: medium=flash|sd picks the store (PUT: flash when the node
// has a flash region, else the card; GET and DELETE: the first store that
// holds the name). A PUT needs its length up front: Content-Length, or
// size=<bytes> with a chunked body. With a token set, requests must carry
// "Authorization: Bearer <token>".

#include "api/http_server.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace kube32::app {

// "/blobs" or "/blobs/..."
bool is_node_blobs_path(std::string_view path);

// Answers one request for `path` ("/blobs/...") on `conn`; its body is
// streamed (HttpHandler::streams_body)
bool serve_node_blobs(api::HttpConnection& conn, std::string_view path, const std::string& token);

} // namespace kube32::app
