// Node storage over the network: /storage on a node's kubelet endpoint (the
// pod HTTP server) and the API server's node proxy in front of it.

#include "pod_test_util.hpp"
#include "app/pod_server.hpp"
#include "app/node_storage_api.hpp"
#include "api/node_proxy.hpp"
#include "api/http_server.hpp"
#include "hal/node_storage.hpp"
#include "kubelet/node_status_reporter.hpp"
#include "storage/store.hpp"

#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>

using namespace kube32;
using k32test::http;

static std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

int main() {
    std::cout << "=== node storage over HTTP ===" << std::endl;
    osal::Socket::init_networking();
    const std::string root = "/tmp/kube32_test_node_storage";
    std::system(("rm -rf " + root).c_str());
    assert(hal::node_storage_mount(false, root));
    const std::string token = "node-token-0123456789";
    const std::string auth = "Authorization: Bearer " + token + "\r\n";

    // The node: its pod HTTP server, small buffers, a 1 MiB upload limit
    app::PodServerOptions po;
    po.port = 18693;
    po.workers = 1;
    po.rx_buffer = 2048;
    po.worker_stack = 262144;
    po.token = token;
    po.max_upload_bytes = 1024 * 1024;
    app::PodServer node(po);
    assert(node.start());

    std::string data(300 * 1024, '\0');
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<char>((i * 7919) >> 3);

    // PUT streams through the 2 KiB buffer into <root>/kube32/<path>
    k32test::HttpReply r = http(po.port, "PUT", "/storage/images/app.wasm", auth, data);
    std::cout << "[put] " << r.status << " " << r.body << std::endl;
    assert(r.status == 201 && r.body.find("\"bytes\":307200") != std::string::npos);
    assert(slurp(root + "/kube32/images/app.wasm") == data);
    r = http(po.port, "PUT", "/storage/images/app.wasm", auth, "v2");
    assert(r.status == 200 && slurp(root + "/kube32/images/app.wasm") == "v2");
    r = http(po.port, "PUT", "/storage/images/app.wasm", auth, data);
    assert(r.status == 200);

    // GET a file, a directory, the root
    r = http(po.port, "GET", "/storage/images/app.wasm", auth);
    assert(r.status == 200 && r.body == data && r.has("Content-Length: 307200"));
    r = http(po.port, "GET", "/storage/images", auth);
    assert(r.status == 200 && r.body.find("{\"name\":\"app.wasm\",\"size\":307200}") != std::string::npos);
    r = http(po.port, "GET", "/storage/", auth);
    assert(r.status == 200 && r.body.find("{\"name\":\"images\",\"dir\":true}") != std::string::npos);

    // Refused: no token, a bad path, too large, PUT on a directory, other methods
    r = http(po.port, "GET", "/storage/images/app.wasm");
    assert(r.status == 401);
    r = http(po.port, "PUT", "/storage/images/x", "Authorization: Bearer wrong-token-000000\r\n", "x");
    assert(r.status == 401);
    r = http(po.port, "GET", "/storage/images/../../etc", auth);
    assert(r.status == 400 || r.status == 404);
    r = http(po.port, "PUT", "/storage/a%2Fb", auth, "x");
    assert(r.status == 400);
    {
        // Refused from the head alone: the body is never read
        osal::Socket s;
        assert(s.create(osal::SocketType::Tcp) && s.connect("127.0.0.1", po.port, 2000));
        s.set_timeout(5000, 5000);
        std::string head = "PUT /storage/big.bin HTTP/1.1\r\nHost: x\r\n" + auth + "Content-Length: 2000000\r\n\r\n";
        assert(s.send_all(head.data(), head.size()));
        char buf[512];
        ssize_t n = s.recv(buf, sizeof buf);
        assert(n > 12 && std::string(buf, 12) == "HTTP/1.1 413");
    }
    r = http(po.port, "PUT", "/storage/images", auth, "x");
    assert(r.status == 409);
    r = http(po.port, "POST", "/storage/images/x", auth, "x");
    assert(r.status == 405);

    // DELETE: files, then the empty directory
    r = http(po.port, "DELETE", "/storage/images", auth);
    assert(r.status == 409);
    r = http(po.port, "DELETE", "/storage/images/app.wasm", auth);
    assert(r.status == 200);
    r = http(po.port, "GET", "/storage/images/app.wasm", auth);
    assert(r.status == 404);
    r = http(po.port, "DELETE", "/storage/images", auth);
    assert(r.status == 200);

    // The kubelet endpoint in the Node's status
    {
        kubelet::NodeStatusReporter rep("w1", "127.0.0.1", false);
        rep.set_kubelet_port(7000);
        std::string reg = rep.build_registration_json(rep.collect_telemetry());
        assert(reg.find("\"daemonEndpoints\":{\"kubeletEndpoint\":{\"Port\":7000}}") != std::string::npos);
    }

    // The API server's node proxy: w1 is the node above, cp the API server's own
    auto store = storage::create_radix_store(storage::StoreOptions{"/tmp/kube32_test_node_proxy", false});
    store->put("/registry/nodes/w1", "{\"kind\":\"Node\",\"metadata\":{\"name\":\"w1\"},\"status\":{\"addresses\":[{\"type\":\"InternalIP\",\"address\":\"127.0.0.1\"}],"
               "\"daemonEndpoints\":{\"kubeletEndpoint\":{\"Port\":" + std::to_string(po.port) + "}}}}", true);
    store->put("/registry/nodes/cp", "{\"kind\":\"Node\",\"metadata\":{\"name\":\"cp\"},\"status\":{}}", true);
    store->put("/registry/nodes/old", "{\"kind\":\"Node\",\"metadata\":{\"name\":\"old\"},\"status\":{\"addresses\":[{\"type\":\"InternalIP\",\"address\":\"127.0.0.1\"}]}}", true);
    api::HttpServerOptions ao;
    ao.port = 18694;
    ao.rx_buffer_size = 4096;
    ao.worker_stack_bytes = 262144;
    ao.watch_streams = 0;
    ao.max_body_size = 64 * 1024;   // uploads stream past it
    api::HttpServer apiserver(ao, *store);
    const std::string api_token = "api-token-abcdefghijklmn";
    apiserver.route_handler().set_bearer_token(api_token);
    std::string local_root = "/tmp/kube32_test_node_proxy_local";
    api::NodeProxyHandler proxy(*store, apiserver.route_handler(), "cp", [&](api::HttpConnection& conn, std::string_view path) {
        if (!app::is_node_storage_path(path)) return false;
        return app::serve_node_storage(conn, path, std::string(), 1024 * 1024);
    });
    proxy.set_node_token(token);
    apiserver.set_handler(&proxy);
    assert(apiserver.start());
    const std::string api_auth = "Authorization: Bearer " + api_token + "\r\n";

    r = http(ao.port, "PUT", "/api/v1/nodes/w1/proxy/storage/images/sdnote.wasm", api_auth, data);
    std::cout << "[proxy put] " << r.status << " " << r.body << std::endl;
    assert(r.status == 201 && slurp(root + "/kube32/images/sdnote.wasm") == data);
    r = http(ao.port, "GET", "/api/v1/nodes/w1/proxy/storage/images/sdnote.wasm", api_auth);
    assert(r.status == 200 && r.body == data);
    r = http(ao.port, "GET", "/api/v1/nodes/w1/proxy/storage/images", api_auth);
    assert(r.status == 200 && r.body.find("sdnote.wasm") != std::string::npos);
    r = http(ao.port, "DELETE", "/api/v1/nodes/w1/proxy/storage/images/sdnote.wasm", api_auth);
    assert(r.status == 200);
    r = http(ao.port, "GET", "/api/v1/nodes/w1/proxy/storage/images/sdnote.wasm");
    assert(r.status == 401);
    r = http(ao.port, "GET", "/api/v1/nodes/nosuch/proxy/storage/", api_auth);
    assert(r.status == 404);
    r = http(ao.port, "GET", "/api/v1/nodes/old/proxy/storage/", api_auth);
    assert(r.status == 503);
    // A chunked body (kubectl replace --raw sends one), re-chunked to the node
    {
        osal::Socket s;
        assert(s.create(osal::SocketType::Tcp) && s.connect("127.0.0.1", ao.port, 2000));
        s.set_timeout(5000, 5000);
        std::string req = "PUT /api/v1/nodes/w1/proxy/storage/chunked.bin HTTP/1.1\r\nHost: x\r\n" + api_auth +
                          "Transfer-Encoding: chunked\r\n\r\n5;ext=1\r\nhello\r\n6\r\n world\r\n0\r\n\r\n";
        assert(s.send_all(req.data(), req.size()));
        std::string resp;
        char buf[1024];
        for (;;) {
            ssize_t n = s.recv(buf, sizeof buf);
            if (n <= 0) break;
            resp.append(buf, static_cast<size_t>(n));
        }
        std::cout << "[chunked put] " << resp.substr(0, resp.find("\r\n")) << std::endl;
        assert(resp.find("HTTP/1.1 201") == 0);
        assert(slurp(root + "/kube32/chunked.bin") == "hello world");
    }

    // The API server's own node: in-process, the same storage root here
    r = http(ao.port, "PUT", "/api/v1/nodes/cp/proxy/storage/local.txt", api_auth, "local");
    assert(r.status == 201 && slurp(root + "/kube32/local.txt") == "local");
    r = http(ao.port, "GET", "/api/v1/nodes/cp/proxy/other", api_auth);
    assert(r.status == 404);
    // Other API paths still reach the Kubernetes router
    r = http(ao.port, "GET", "/api/v1/nodes/w1", api_auth);
    assert(r.status == 200 && r.body.find("\"name\":\"w1\"") != std::string::npos);

    apiserver.stop();
    node.stop();
    std::cout << "[PASS] node storage: PUT/GET/DELETE, limits, auth, node proxy" << std::endl;
    return 0;
}
