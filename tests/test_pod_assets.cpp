// k32_resp_asset: bundle files of a pod's image sent as the response, from an
// embedded bundle (images/web/<name>/) or a directory on node storage.

#include "pod_test_util.hpp"
#include "app/pod_server.hpp"
#include "runtime/k32_app_abi.h"
#include "embedded/embedded_blobs.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sys/stat.h>

using namespace kube32;
using k32test::call_pod;

static void write_file(const std::string& path, const std::string& data) {
    std::ofstream f(path, std::ios::binary);
    f << data;
}

int main() {
    std::cout << "=== k32_resp_asset ===" << std::endl;
    osal::Socket::init_networking();
    auto rt = runtime::create_wasm_runtime();
    const std::vector<uint8_t> probe = k32test::svcprobe();

    // Embedded bundle "echo": index.html stored plain and gzip-compressed
    const embedded::EmbeddedAsset* plain = nullptr;
    const embedded::EmbeddedAsset* gz = nullptr;
    size_t count = 0;
    const embedded::EmbeddedAsset* all = embedded::assets(count);
    for (size_t i = 0; i < count; ++i) {
        if (std::strcmp(all[i].bundle, "echo") == 0 && std::strcmp(all[i].path, "/index.html") == 0) (all[i].gzip ? gz : plain) = &all[i];
    }
    assert(plain && gz);
    runtime::DeploySpec spec;
    spec.asset_bundle = "echo";
    k32test::deploy(*rt, "default/web-0", probe, 16 * 1024, "", spec);

    k32test::Collect c = call_pod("default/web-0", "/asset/index.html");
    assert(c.status == 200 && c.ctype == "text/html; charset=utf-8");
    assert(c.body == std::string(reinterpret_cast<const char*>(plain->data), plain->len));
    assert(c.announced == plain->len && c.ended);
    assert(c.headers.find("Cache-Control: no-cache") != std::string::npos);   // set before the call
    assert(c.headers.find("Content-Encoding") == std::string::npos);
    c = call_pod("default/web-0", "/asset/index.html", "GET", "", true);
    assert(c.status == 200 && c.headers.find("Content-Encoding: gzip") != std::string::npos);
    assert(c.body.size() == gz->len && static_cast<unsigned char>(c.body[0]) == 0x1f);
    // No such file: nothing sent; the guest withdraws the header it set
    // (an empty value) and answers itself
    c = call_pod("default/web-0", "/asset/missing.js");
    assert(c.status == 404 && c.body.find("\"rc\":-2") != std::string::npos);
    assert(c.headers.find("Cache-Control") == std::string::npos);
    c = call_pod("default/web-0", "/asset/../secret");
    assert(c.status == 404);

    // A bundle with gzip files only answers 406 to clients without gzip
    runtime::DeploySpec gz_only;
    gz_only.asset_bundle = "tessera-web";
    k32test::deploy(*rt, "default/gzonly-0", probe, 16 * 1024, "", gz_only);
    c = call_pod("default/gzonly-0", "/asset/app.js");
    assert(c.status == 406);
    c = call_pod("default/gzonly-0", "/asset/app.js", "GET", "", true);
    assert(c.status == 200 && c.ctype == "application/javascript; charset=utf-8" && c.headers.find("Content-Encoding: gzip") != std::string::npos);

    // A pod without a bundle (inline image)
    k32test::deploy(*rt, "default/nobundle-0", probe, 16 * 1024);
    c = call_pod("default/nobundle-0", "/asset/index.html");
    assert(c.status == 404);

    // A bundle directory on node storage (wasm://sd/<name>): read through one small buffer
    const std::string dir = "/tmp/kube32_test_assets_bundle";
    mkdir(dir.c_str(), 0755);
    mkdir((dir + "/css").c_str(), 0755);
    std::string big(100000, '\0');
    for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>(i * 31 + 7);
    write_file(dir + "/data.bin", big);
    write_file(dir + "/css/site.css", "body{}");
    write_file(dir + "/app.js", "plain();");
    write_file(dir + "/app.js.gz", "\x1f\x8b-compressed");
    runtime::DeploySpec sd;
    sd.asset_directory = dir;
    k32test::deploy(*rt, "default/sd-0", probe, 16 * 1024, "", sd);
    c = call_pod("default/sd-0", "/asset/data.bin");
    assert(c.status == 200 && c.body == big && c.announced == big.size() && c.ctype == "application/octet-stream");
    c = call_pod("default/sd-0", "/asset/css/site.css");
    assert(c.status == 200 && c.body == "body{}" && c.ctype == "text/css; charset=utf-8");
    c = call_pod("default/sd-0", "/asset/app.js");
    assert(c.status == 200 && c.body == "plain();");
    c = call_pod("default/sd-0", "/asset/app.js", "GET", "", true);
    assert(c.status == 200 && c.body == "\x1f\x8b-compressed" && c.headers.find("Content-Encoding: gzip") != std::string::npos);
    c = call_pod("default/sd-0", "/asset/nope.txt");
    assert(c.status == 404);

    // Through the pod HTTP server: Content-Length framing, gzip by Accept-Encoding, HEAD
    app::PodServerOptions po;
    po.port = 18691;
    po.worker_stack = 262144;
    app::PodServer server(po);
    assert(server.start());
    k32test::HttpReply h = k32test::http(po.port, "GET", "/asset/index.html", "X-Kube32-Pod: default/web-0\r\nAccept-Encoding: gzip, br\r\n");
    std::cout << "[http] " << h.head.substr(0, h.head.find("\r\n")) << ", " << h.body.size() << " bytes" << std::endl;
    assert(h.status == 200 && h.has("Content-Encoding: gzip") && h.has("Content-Length: " + std::to_string(gz->len)));
    assert(!h.has("Transfer-Encoding") && h.body.size() == gz->len);
    h = k32test::http(po.port, "GET", "/asset/data.bin", "X-Kube32-Pod: default/sd-0\r\n");
    assert(h.status == 200 && h.body == big && h.has("Content-Length: 100000"));
    h = k32test::http(po.port, "HEAD", "/asset/data.bin", "X-Kube32-Pod: default/sd-0\r\n");
    assert(h.status == 200 && h.has("Content-Length: 100000") && h.body.empty());
    server.stop();

    std::cout << "[PASS] k32_resp_asset: embedded and storage bundles, gzip variants, headers" << std::endl;
    return 0;
}
