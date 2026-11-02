// Inline images (api/inline_images.hpp): a module a Pod or workload carries
// in kube32.io/wasm-base64 goes to the image store as the body is read, and
// the object keeps kube32.io/wasm-sha256 and kube32.io/wasm-size. kubectl
// create, apply, replace and patch, dry runs, Deployments and Jobs, objects
// stored before, the HTTP server's streaming filter and the kubelet's read
// of a module by its digest.

#include "api/http_server.hpp"
#include "api/inline_images.hpp"
#include "api/route_handler.hpp"
#include "kubelet/kube_api_client.hpp"
#include "mem/monotonic_arena.hpp"
#include "models/kube_models.hpp"
#include "osal/sha256.hpp"
#include "osal/socket.hpp"
#include "storage/flash_log.hpp"
#include "storage/store.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace kube32;

static std::vector<uint8_t> module_bytes(size_t n, int seed) {
    std::vector<uint8_t> m(n);
    uint32_t x = 2166136261u ^ static_cast<uint32_t>(seed);
    for (size_t i = 0; i < n; ++i) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        m[i] = static_cast<uint8_t>(x);
    }
    return m;
}

// Base64; `wrap` > 0 breaks lines as a YAML block scalar would (\n in JSON)
static std::string base64(const std::vector<uint8_t>& in, size_t wrap = 0) {
    static const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    auto put = [&](char c) {
        out += c;
        if (wrap && (out.size() + 2) % (wrap + 2) == 0) out += "\\n";
    };
    for (; i + 2 < in.size(); i += 3) {
        uint32_t n = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
        put(a[n >> 18]); put(a[(n >> 12) & 63]); put(a[(n >> 6) & 63]); put(a[n & 63]);
    }
    if (i < in.size()) {
        uint32_t n = in[i] << 16 | (i + 1 < in.size() ? in[i + 1] << 8 : 0);
        put(a[n >> 18]); put(a[(n >> 12) & 63]);
        put((i + 1 < in.size()) ? a[(n >> 6) & 63] : '=');
        put('=');
    }
    return out;
}

static std::string sha256(const std::vector<uint8_t>& m) { return osal::Sha256::hex(osal::Sha256::of(m.data(), m.size())); }

// JSON text as a JSON string value (for last-applied-configuration)
static std::string quoted(const std::string& json) {
    std::string out = "\"";
    for (char c : json) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

static std::string pod(const std::string& name, const std::string& annotations, const std::string& node = "") {
    return "{\"apiVersion\":\"v1\",\"kind\":\"Pod\",\"metadata\":{\"name\":\"" + name + "\",\"namespace\":\"default\",\"annotations\":{" +
           annotations + "}},\"spec\":{" + (node.empty() ? std::string() : "\"nodeName\":\"" + node + "\",") +
           "\"containers\":[{\"name\":\"c\",\"image\":\"wasm://inline/" + name + "\"}]}}";
}

static std::string inline_annotation(const std::vector<uint8_t>& m) { return "\"kube32.io/wasm-base64\":\"" + base64(m) + "\""; }

static std::unique_ptr<storage::Store> journal_store(storage::RamFlashRegion& flash) {
    storage::StoreOptions o;
    o.enable_flash_persistence = true;
    o.flash_region = &flash;
    auto store = storage::create_radix_store(o);
    assert(store->recover().ok());
    return store;
}

static size_t count(storage::Store& store, const std::string& prefix) {
    std::vector<std::string> values, keys;
    store.list_extended(prefix, values, &keys);
    return keys.size();
}

static size_t occurrences(const std::string& text, const std::string& what) {
    size_t n = 0;
    for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++n;
    return n;
}

static api::HttpResponse call(api::RouteHandler& h, api::HttpMethod method, const std::string& uri, const std::string& body = "",
                              const std::string& content_type = "application/json") {
    api::HttpRequest req;
    req.method = method;
    req.raw_uri = uri;
    api::normalize_uri(uri, req.path, req.query_string);
    req.body = body;
    req.content_type = content_type;
    mem::MonotonicArena arena;
    return h.handle_request(req, arena);
}

static std::string annotation(const std::string& object_json, const std::string& key) {
    models::JsonValue doc;
    std::string err;
    assert(models::parse_json(object_json, doc, err));
    const models::JsonValue* meta = doc.get("metadata");
    const models::JsonValue* ann = meta ? meta->get("annotations") : nullptr;
    const models::JsonValue* v = ann ? ann->get(key) : nullptr;
    return v && v->is_string() ? v->str_val : std::string("<none>");
}

static void test_filter_takes_values_out_as_they_stream() {
    std::cout << "[RUN] test_filter_takes_values_out_as_they_stream" << std::endl;
    storage::RamFlashRegion flash(256 * 4096);
    auto store = journal_store(flash);
    api::InlineImages images(*store);
    const std::vector<uint8_t> m = module_bytes(20000, 1);
    const std::string d = sha256(m);
    // The module twice: in the annotation and, escaped, in kubectl apply's record of the manifest
    const std::string inner = pod("app", inline_annotation(m));
    const std::string body = pod("app", inline_annotation(m) + ",\"kubectl.kubernetes.io/last-applied-configuration\":" + quoted(inner));

    std::string whole;
    {
        api::InlineImageFilter f(images, true);
        assert(f.feed(body.data(), body.size(), whole) && f.finish());
        assert(f.images() == 2);
    }
    assert(whole.find(base64(m).substr(0, 40)) == std::string::npos);
    assert(occurrences(whole, "\"kube32.io/wasm-base64\":\"sha256:" + d + "\"") == 1);
    assert(occurrences(whole, "\\\"kube32.io/wasm-base64\\\":\\\"sha256:" + d + "\\\"") == 1);
    assert(whole.size() < 1200);
    // One image for both, its pieces on flash
    assert(count(*store, "/images/") == 1);
    assert(count(*store, "/blobs/") == (m.size() + api::InlineImages::kPieceBytes - 1) / api::InlineImages::kPieceBytes);
    std::vector<uint8_t> back;
    std::string why;
    assert(images.read(d, [&](const char* p, size_t n) { back.insert(back.end(), p, p + n); return true; }, why) && back == m);

    // Fed a byte at a time the result is the same
    std::string bytewise;
    {
        api::InlineImageFilter f(images, true);
        for (char c : body) assert(f.feed(&c, 1, bytewise));
        assert(f.finish());
    }
    assert(bytewise == whole);
    assert(count(*store, "/images/") == 1);

    // Whitespace, an escaped '/', a value wrapped over lines (YAML block scalar)
    const std::string loose = "{\"metadata\":{\"annotations\":{\"kube32.io/wasm-base64\" :\n \"" + base64(m, 76) + "\"}}}";
    std::string out;
    std::string slashed = loose;
    for (size_t at = slashed.find('/', slashed.find(" :")); at != std::string::npos; at = slashed.find('/', at + 2)) slashed.replace(at, 1, "\\/");
    for (const std::string* text : {&loose, static_cast<const std::string*>(&slashed)}) {
        out.clear();
        api::InlineImageFilter f(images, true);
        assert(f.feed(text->data(), text->size(), out) && f.finish());
        assert(out.find("\"sha256:" + d + "\"") != std::string::npos);
    }

    // References and empty values go through untouched; a dry run stores nothing
    const std::string refs = "{\"a\":{\"kube32.io/wasm-base64\":\"sha256:" + d + "\",\"b\":{\"kube32.io/wasm-base64\":\"\"}}}";
    out.clear();
    {
        api::InlineImageFilter f(images, true);
        assert(f.feed(refs.data(), refs.size(), out) && f.finish() && f.images() == 0);
    }
    assert(out == refs);
    const std::vector<uint8_t> other = module_bytes(5000, 2);
    const std::string dry = pod("dry", inline_annotation(other));
    out.clear();
    {
        api::InlineImageFilter f(images, false);
        assert(f.feed(dry.data(), dry.size(), out) && f.finish());
    }
    assert(out.find("sha256:" + sha256(other)) != std::string::npos);
    assert(count(*store, "/images/") == 1 && !images.read(sha256(other), [](const char*, size_t) { return true; }, why));
    std::cout << "[PASS] test_filter_takes_values_out_as_they_stream" << std::endl;
}

static void test_filter_refuses_what_is_not_a_module() {
    std::cout << "[RUN] test_filter_refuses_what_is_not_a_module" << std::endl;
    storage::RamFlashRegion flash(256 * 4096);
    auto store = journal_store(flash);
    api::InlineImages images(*store);
    const std::vector<uint8_t> m = module_bytes(30000, 3);
    std::string b64 = base64(m);
    b64[25000] = '!';
    const std::string bad = pod("bad", "\"kube32.io/wasm-base64\":\"" + b64 + "\"");
    std::string out;
    {
        api::InlineImageFilter f(images, true);
        assert(!f.feed(bad.data(), bad.size(), out));
        assert(f.status() == 422 && f.error().find("not base64") != std::string::npos);
    }
    // What the refused upload stored is gone
    assert(count(*store, "/blobs/") == 0 && count(*store, "/images/") == 0);
    // A body that ends inside the value
    const std::string good = pod("good", inline_annotation(m));
    out.clear();
    {
        api::InlineImageFilter f(images, true);
        assert(f.feed(good.data(), good.size() / 2, out));
        assert(!f.finish() && f.status() == 400);
    }
    assert(count(*store, "/blobs/") == 0 && count(*store, "/images/") == 0);
    std::cout << "[PASS] test_filter_refuses_what_is_not_a_module" << std::endl;
}

static void test_kubectl_create_apply_replace_delete() {
    std::cout << "[RUN] test_kubectl_create_apply_replace_delete" << std::endl;
    storage::RamFlashRegion flash(256 * 4096);
    auto store = journal_store(flash);
    api::RouteHandler h(*store);
    h.inline_images().set_grace_ms(0);
    const std::vector<uint8_t> m1 = module_bytes(40000, 4);
    const std::vector<uint8_t> m2 = module_bytes(12000, 5);
    const std::string d1 = sha256(m1), d2 = sha256(m2);

    // kubectl create
    api::HttpResponse r = call(h, api::HttpMethod::Post, "/api/v1/namespaces/default/pods", pod("app", inline_annotation(m1)));
    assert(r.status_code == 201);
    assert(annotation(r.body, "kube32.io/wasm-base64") == "<none>");
    assert(annotation(r.body, "kube32.io/wasm-sha256") == d1);
    assert(annotation(r.body, "kube32.io/wasm-size") == std::to_string(m1.size()));
    std::string stored;
    assert(store->get("/registry/pods/default/app", stored).ok() && stored.size() < 1024);
    r = call(h, api::HttpMethod::Get, "/api/v1/namespaces/default/pods/app");
    assert(r.status_code == 200 && annotation(r.body, "kube32.io/wasm-sha256") == d1);

    // The module, for kubelets
    r = call(h, api::HttpMethod::Get, "/apis/kube32.io/v1/images/" + d1);
    assert(r.status_code == 200 && r.content_type == "application/wasm");
    assert(r.body == std::string(m1.begin(), m1.end()));
    assert(call(h, api::HttpMethod::Get, "/apis/kube32.io/v1/images/" + d2).status_code == 404);
    assert(call(h, api::HttpMethod::Delete, "/apis/kube32.io/v1/images/" + d1).status_code == 405);

    // kubectl apply on it: a strategic merge patch with the module in the
    // annotation and in last-applied-configuration
    const std::string applied = pod("app", inline_annotation(m2));
    const std::string patch = "{\"metadata\":{\"annotations\":{" + inline_annotation(m2) +
                              ",\"kubectl.kubernetes.io/last-applied-configuration\":" + quoted(applied) + "}}}";
    r = call(h, api::HttpMethod::Patch, "/api/v1/namespaces/default/pods/app", patch, "application/strategic-merge-patch+json");
    assert(r.status_code == 200);
    assert(annotation(r.body, "kube32.io/wasm-sha256") == d2 && annotation(r.body, "kube32.io/wasm-size") == std::to_string(m2.size()));
    assert(annotation(r.body, "kube32.io/wasm-base64") == "<none>");
    const std::string last = annotation(r.body, "kubectl.kubernetes.io/last-applied-configuration");
    assert(last.find("\"kube32.io/wasm-base64\":\"sha256:" + d2 + "\"") != std::string::npos);
    assert(store->get("/registry/pods/default/app", stored).ok() && stored.size() < 2048);
    // Applying the same manifest again sends the module again: same digest, nothing new
    r = call(h, api::HttpMethod::Patch, "/api/v1/namespaces/default/pods/app", patch, "application/strategic-merge-patch+json");
    assert(r.status_code == 200 && count(*store, "/images/") == 2);
    // The first module is referred to by nothing now
    assert(h.inline_images().collect_garbage() == 1);
    assert(count(*store, "/images/") == 1 && call(h, api::HttpMethod::Get, "/apis/kube32.io/v1/images/" + d1).status_code == 404);

    // kubectl replace (PUT) with the module inline
    r = call(h, api::HttpMethod::Put, "/api/v1/namespaces/default/pods/app", pod("app", inline_annotation(m1)));
    assert(r.status_code == 200 && annotation(r.body, "kube32.io/wasm-sha256") == d1);

    // What is refused: not base64, a reference to an image that is not stored
    std::string bad = base64(m2);
    bad[100] = '*';
    r = call(h, api::HttpMethod::Post, "/api/v1/namespaces/default/pods", pod("bad", "\"kube32.io/wasm-base64\":\"" + bad + "\""));
    assert(r.status_code == 422 && r.body.find("\"reason\":\"Invalid\"") != std::string::npos);
    r = call(h, api::HttpMethod::Post, "/api/v1/namespaces/default/pods",
             pod("ref", "\"kube32.io/wasm-base64\":\"sha256:" + std::string(64, 'e') + "\""));
    assert(r.status_code == 422);
    assert(store->get("/registry/pods/default/bad", stored).code == storage::StatusCode::NotFound);
    // A reference to a stored image is taken as one
    r = call(h, api::HttpMethod::Post, "/api/v1/namespaces/default/pods", pod("ref", "\"kube32.io/wasm-base64\":\"sha256:" + d1 + "\""));
    assert(r.status_code == 201 && annotation(r.body, "kube32.io/wasm-sha256") == d1);

    // A dry run checks and stores nothing
    const std::vector<uint8_t> m3 = module_bytes(9000, 6);
    const size_t images_before = count(*store, "/images/");
    r = call(h, api::HttpMethod::Post, "/api/v1/namespaces/default/pods?dryRun=All", pod("dry", inline_annotation(m3)));
    assert(r.status_code == 201 && annotation(r.body, "kube32.io/wasm-sha256") == sha256(m3));
    assert(count(*store, "/images/") == images_before);

    // Deployments and Jobs: the pod template carries the digest, the pods
    // made from it too
    const std::string tmpl = "\"template\":{\"metadata\":{\"labels\":{\"app\":\"web\"},\"annotations\":{" + inline_annotation(m3) +
                             "}},\"spec\":{\"containers\":[{\"name\":\"c\",\"image\":\"wasm://inline/web\"}]}}";
    r = call(h, api::HttpMethod::Post, "/apis/apps/v1/namespaces/default/deployments",
             "{\"apiVersion\":\"apps/v1\",\"kind\":\"Deployment\",\"metadata\":{\"name\":\"web\"},\"spec\":{\"replicas\":1,"
             "\"selector\":{\"matchLabels\":{\"app\":\"web\"}}," + tmpl + "}}");
    assert(r.status_code == 201);
    assert(store->get("/registry/deployments/default/web", stored).ok());
    assert(stored.find("\"kube32.io/wasm-sha256\":\"" + sha256(m3) + "\"") != std::string::npos && stored.find("wasm-base64") == std::string::npos);
    r = call(h, api::HttpMethod::Post, "/apis/batch/v1/namespaces/default/jobs",
             "{\"apiVersion\":\"batch/v1\",\"kind\":\"Job\",\"metadata\":{\"name\":\"once\"},\"spec\":{\"template\":{\"metadata\":{\"annotations\":{" +
             inline_annotation(m2) + "}},\"spec\":{\"restartPolicy\":\"Never\",\"containers\":[{\"name\":\"c\",\"image\":\"wasm://inline/once\"}]}}}}");
    assert(r.status_code == 201);
    assert(store->get("/registry/jobs/default/once", stored).ok() && stored.find("\"kube32.io/wasm-sha256\":\"" + d2 + "\"") != std::string::npos);

    // kubectl delete: an image goes with the last object that refers to it
    const size_t with_all = count(*store, "/images/");
    assert(call(h, api::HttpMethod::Delete, "/api/v1/namespaces/default/pods/ref").status_code == 200);
    assert(count(*store, "/images/") == with_all);                     // app still refers to m1
    assert(call(h, api::HttpMethod::Delete, "/api/v1/namespaces/default/pods/app").status_code == 200);
    assert(count(*store, "/images/") == with_all - 1);
    assert(call(h, api::HttpMethod::Delete, "/apis/apps/v1/namespaces/default/deployments/web").status_code == 200);
    assert(call(h, api::HttpMethod::Delete, "/apis/batch/v1/namespaces/default/jobs/once").status_code == 200);
    assert(count(*store, "/images/") == 0 && count(*store, "/blobs/") == 0);
    std::cout << "[PASS] test_kubectl_create_apply_replace_delete" << std::endl;
}

static void test_objects_stored_before_give_up_their_images() {
    std::cout << "[RUN] test_objects_stored_before_give_up_their_images" << std::endl;
    storage::RamFlashRegion flash(256 * 4096);
    const std::vector<uint8_t> m = module_bytes(15000, 7);
    {
        auto store = journal_store(flash);
        assert(store->put("/registry/pods/default/old", pod("old", inline_annotation(m) + ",\"kube32.io/wasm-size\":\"1\"", "n1"), true).ok());
        assert(store->put("/registry/pods/default/plain", pod("plain", "\"a\":\"b\""), true).ok());
        // Pieces of an upload a restart cut short
        assert(store->put_extended("/blobs/0123456789abcdef/0", std::string(100, 'x'), true, true).ok());
        assert(store->commit().ok());
    }
    auto store = journal_store(flash);
    api::RouteHandler h(*store);
    std::string stored;
    assert(store->get("/registry/pods/default/old", stored).ok());
    assert(stored.size() < 1024 && stored.find("wasm-base64") == std::string::npos);
    assert(annotation(stored, "kube32.io/wasm-sha256") == sha256(m) && annotation(stored, "kube32.io/wasm-size") == std::to_string(m.size()));
    assert(store->get("/blobs/0123456789abcdef/0", stored).code == storage::StatusCode::NotFound);
    api::HttpResponse r = call(h, api::HttpMethod::Get, "/apis/kube32.io/v1/images/" + sha256(m));
    assert(r.status_code == 200 && r.body == std::string(m.begin(), m.end()));
    std::cout << "[PASS] test_objects_stored_before_give_up_their_images" << std::endl;
}

// One raw request; returns the status line's code and the body
static int raw_call(uint16_t port, const std::string& head, const std::string& body, std::string* out_body = nullptr) {
    osal::Socket s;
    assert(s.create(osal::SocketType::Tcp) && s.connect("127.0.0.1", port, 2000));
    s.set_timeout(5000, 5000);
    assert(s.send_all(head.data(), head.size()));
    if (!body.empty()) s.send_all(body.data(), body.size());
    std::string got;
    char buf[4096];
    for (;;) {
        ssize_t n = s.recv(buf, sizeof buf);
        if (n <= 0) break;
        got.append(buf, static_cast<size_t>(n));
    }
    const size_t at = got.find("\r\n\r\n");
    if (out_body) *out_body = at == std::string::npos ? std::string() : got.substr(at + 4);
    return got.size() > 12 ? std::atoi(got.c_str() + 9) : 0;
}

static void test_server_filters_bodies_and_kubelets_read_modules() {
    std::cout << "[RUN] test_server_filters_bodies_and_kubelets_read_modules" << std::endl;
    osal::Socket::init_networking();
    storage::RamFlashRegion flash(512 * 4096);
    auto store = journal_store(flash);
    api::HttpServerOptions o;
    o.host = "127.0.0.1";
    o.port = 18781;
    o.workers = 2;
    o.rx_buffer_size = 4096;
    o.worker_stack_bytes = 262144;
    o.watch_streams = 0;
    api::HttpServer server(o, *store);
    assert(server.start());

    const std::vector<uint8_t> m = module_bytes(66000, 8);
    const std::string d = sha256(m);
    const std::string body = pod("big", inline_annotation(m), "n1");
    std::string reply;
    int status = raw_call(o.port, "POST /api/v1/namespaces/default/pods HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\nContent-Length: " +
                                      std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n", body, &reply);
    assert(status == 201 && annotation(reply, "kube32.io/wasm-sha256") == d);
    // A chunked body (kubectl create --raw)
    const std::vector<uint8_t> m2 = module_bytes(20000, 9);
    const std::string body2 = pod("chunky", inline_annotation(m2), "n1");
    std::string chunked;
    for (size_t at = 0; at < body2.size(); at += 3000) {
        const std::string part = body2.substr(at, 3000);
        char line[16];
        snprintf(line, sizeof line, "%zx\r\n", part.size());
        chunked += line + part + "\r\n";
    }
    chunked += "0\r\n\r\n";
    status = raw_call(o.port, "POST /api/v1/namespaces/default/pods HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\n"
                              "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n", chunked, &reply);
    assert(status == 201 && annotation(reply, "kube32.io/wasm-sha256") == sha256(m2));
    // Not base64: a Status that says so, nothing stored
    std::string bad = base64(m2);
    bad[5000] = '#';
    const std::string body3 = pod("bad", "\"kube32.io/wasm-base64\":\"" + bad + "\"");
    status = raw_call(o.port, "POST /api/v1/namespaces/default/pods HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\nContent-Length: " +
                                      std::to_string(body3.size()) + "\r\n\r\n", body3, &reply);
    assert(status == 422 && reply.find("not base64") != std::string::npos);
    assert(count(*store, "/images/") == 2);

    // The kubelet reads the module by its digest, checked against it
    kubelet::HttpKubeClient client("127.0.0.1", o.port);
    runtime::ModuleBytes got;
    std::string why;
    assert(client.get_image(d, got, why) && std::vector<uint8_t>(got.begin(), got.end()) == m);
    assert(!client.get_image(std::string(64, 'a'), got, why) && why.find("404") != std::string::npos && got.empty());
    assert(!client.get_image("not-a-digest", got, why));
    server.stop();

    // A server that answers with other bytes than the digest names
    {
        osal::Socket l;
        assert(l.create(osal::SocketType::Tcp));
        l.set_reuse_addr(true);
        assert(l.bind("127.0.0.1", 18782) && l.listen(1));
        std::thread srv([&l]() {
            osal::Socket c = l.accept();
            char b[1024];
            c.recv(b, sizeof b);
            const std::string r = "HTTP/1.1 200 OK\r\nContent-Type: application/wasm\r\nContent-Length: 5\r\nConnection: close\r\n\r\nabcde";
            c.send_all(r.data(), r.size());
            c.close();
        });
        kubelet::HttpKubeClient liar("127.0.0.1", 18782);
        assert(!liar.get_image(d, got, why) && why.find("do not match") != std::string::npos && got.empty());
        srv.join();
        l.close();
    }

    // With a token, an unauthenticated body is not taken in
    api::HttpServerOptions o2 = o;
    o2.port = 18783;
    api::HttpServer locked(o2, *store);
    locked.route_handler().set_bearer_token("0123456789abcdef0123");
    assert(locked.start());
    const std::vector<uint8_t> m4 = module_bytes(3000, 10);
    const std::string body4 = pod("anon", inline_annotation(m4));
    status = raw_call(o2.port, "POST /api/v1/namespaces/default/pods HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\nContent-Length: " +
                                       std::to_string(body4.size()) + "\r\nConnection: close\r\n\r\n", body4);
    assert(status == 401 && count(*store, "/images/") == 2);
    locked.stop();
    std::cout << "[PASS] test_server_filters_bodies_and_kubelets_read_modules" << std::endl;
}

int main() {
    std::cout << "=== inline images ===" << std::endl;
    test_filter_takes_values_out_as_they_stream();
    test_filter_refuses_what_is_not_a_module();
    test_kubectl_create_apply_replace_delete();
    test_objects_stored_before_give_up_their_images();
    test_server_filters_bodies_and_kubelets_read_modules();
    std::cout << "All inline image tests passed" << std::endl;
    return 0;
}
