#include "api/http_types.hpp"
#include "api/route_handler.hpp"
#include "api/table_serializer.hpp"
#include "api/chunked_stream_writer.hpp"
#include "storage/store.hpp"
#include "mem/monotonic_arena.hpp"

#include <cassert>
#include <iostream>
#include <vector>
#include <string>

using namespace kube32::api;
using namespace kube32::storage;
using namespace kube32::mem;

class TestBufferSink : public ISink {
public:
    std::string data;
    std::vector<std::string> chunks;

    bool write(const void* buf, size_t len) override {
        std::string s(static_cast<const char*>(buf), len);
        data += s;
        chunks.push_back(s);
        return true;
    }
};

void test_uri_normalization() {
    std::cout << "[RUN] test_uri_normalization" << std::endl;
    std::string path, query;

    normalize_uri("/version?timeout=32s", path, query);
    assert(path == "/version");
    assert(query == "timeout=32s");

    normalize_uri("/api/v1/nodes/?limit=500", path, query);
    assert(path == "/api/v1/nodes");
    assert(query == "limit=500");

    normalize_uri("/api/v1/namespaces/default/pods/", path, query);
    assert(path == "/api/v1/namespaces/default/pods");
    assert(query.empty());

    normalize_uri("/", path, query);
    assert(path == "/");
    assert(query.empty());

    std::cout << "[PASS] test_uri_normalization" << std::endl;
}

void test_route_pattern_matching() {
    std::cout << "[RUN] test_route_pattern_matching" << std::endl;

    assert(RouteHandler::match_route("/version").category == RouteCategory::DiscoveryVersion);
    assert(RouteHandler::match_route("/api").category == RouteCategory::DiscoveryApi);
    assert(RouteHandler::match_route("/apis").category == RouteCategory::DiscoveryApis);
    assert(RouteHandler::match_route("/api/v1").category == RouteCategory::DiscoveryApiV1);
    assert(RouteHandler::match_route("/apis/apps/v1").category == RouteCategory::DiscoveryApisAppsV1);
    assert(RouteHandler::match_route("/openapi/v3").category == RouteCategory::DiscoveryOpenApiV3);

    assert(RouteHandler::match_route("/api/v1/nodes").category == RouteCategory::NodesCollection);
    RouteMatch nm = RouteHandler::match_route("/api/v1/nodes/esp32-node-01");
    assert(nm.category == RouteCategory::NodeItem);
    assert(nm.resource_name == "esp32-node-01");

    RouteMatch nsm = RouteHandler::match_route("/api/v1/nodes/esp32-node-01/status");
    assert(nsm.category == RouteCategory::NodeStatus);
    assert(nsm.resource_name == "esp32-node-01");
    assert(nsm.subresource == "status");

    assert(RouteHandler::match_route("/api/v1/pods").category == RouteCategory::PodsClusterCollection);
    RouteMatch pm1 = RouteHandler::match_route("/api/v1/namespaces/test-ns/pods");
    assert(pm1.category == RouteCategory::PodsNsCollection);
    assert(pm1.namespace_name == "test-ns");

    RouteMatch pm2 = RouteHandler::match_route("/api/v1/namespaces/test-ns/pods/my-pod");
    assert(pm2.category == RouteCategory::PodItem);
    assert(pm2.namespace_name == "test-ns");
    assert(pm2.resource_name == "my-pod");

    RouteMatch pm3 = RouteHandler::match_route("/api/v1/namespaces/test-ns/pods/my-pod/status");
    assert(pm3.category == RouteCategory::PodStatus);
    assert(pm3.namespace_name == "test-ns");
    assert(pm3.resource_name == "my-pod");

    RouteMatch dm1 = RouteHandler::match_route("/apis/apps/v1/namespaces/test-ns/deployments");
    assert(dm1.category == RouteCategory::DeploymentsNsCollection);

    RouteMatch dm2 = RouteHandler::match_route("/apis/apps/v1/namespaces/test-ns/deployments/my-deploy");
    assert(dm2.category == RouteCategory::DeploymentItem);

    RouteMatch dm3 = RouteHandler::match_route("/apis/apps/v1/namespaces/test-ns/deployments/my-deploy/scale");
    assert(dm3.category == RouteCategory::DeploymentScale);

    assert(RouteHandler::match_route("/unrecognized/path").category == RouteCategory::NotFound);

    std::cout << "[PASS] test_route_pattern_matching" << std::endl;
}

void test_discovery_and_method_enforcement() {
    std::cout << "[RUN] test_discovery_and_method_enforcement" << std::endl;
    StoreOptions opts;
    opts.enable_flash_persistence = false;
    auto store = create_radix_store(opts);
    RouteHandler handler(*store);
    MonotonicArena arena;

    // 1. GET /version -> 200
    HttpRequest r1;
    r1.method = HttpMethod::Get;
    r1.path = "/version";
    HttpResponse resp1 = handler.handle_request(r1, arena);
    assert(resp1.status_code == 200);
    assert(resp1.body.find("\"gitVersion\":\"v1.33.0-kube32\"") != std::string::npos);

    // 2. POST /version -> 405 MethodNotAllowed
    HttpRequest r2;
    r2.method = HttpMethod::Post;
    r2.path = "/version";
    HttpResponse resp2 = handler.handle_request(r2, arena);
    assert(resp2.status_code == 405);
    assert(resp2.body.find("\"reason\":\"MethodNotAllowed\"") != std::string::npos);

    // 3. DELETE /api/v1 -> 405 MethodNotAllowed
    HttpRequest r3;
    r3.method = HttpMethod::Delete;
    r3.path = "/api/v1";
    HttpResponse resp3 = handler.handle_request(r3, arena);
    assert(resp3.status_code == 405);

    // 4. PUT /api/v1/nodes -> 405 MethodNotAllowed
    HttpRequest r4;
    r4.method = HttpMethod::Put;
    r4.path = "/api/v1/nodes";
    HttpResponse resp4 = handler.handle_request(r4, arena);
    assert(resp4.status_code == 405);

    // 5. POST on node item instance -> 405
    HttpRequest r5;
    r5.method = HttpMethod::Post;
    r5.path = "/api/v1/nodes/esp32-node-01";
    HttpResponse resp5 = handler.handle_request(r5, arena);
    assert(resp5.status_code == 405);

    // 6. GET on non-existent endpoint -> 404
    HttpRequest r6;
    r6.method = HttpMethod::Get;
    r6.path = "/api/v1/namespaces/default/ghost";
    HttpResponse resp6 = handler.handle_request(r6, arena);
    assert(resp6.status_code == 404);
    assert(resp6.body.find("\"reason\":\"NotFound\"") != std::string::npos);

    // 7. GET /openapi/v3 -> 200 {"paths":{}}
    HttpRequest r7;
    r7.method = HttpMethod::Get;
    r7.path = "/openapi/v3";
    HttpResponse resp7 = handler.handle_request(r7, arena);
    assert(resp7.status_code == 200);
    assert(resp7.body == "{\"paths\":{}}");

    // 8. GET /apis/events.k8s.io/v1 -> 200 APIResourceList
    HttpRequest r8;
    r8.method = HttpMethod::Get;
    r8.path = "/apis/events.k8s.io/v1";
    HttpResponse resp8 = handler.handle_request(r8, arena);
    assert(resp8.status_code == 200);
    assert(resp8.body.find("\"groupVersion\":\"events.k8s.io/v1\"") != std::string::npos);
    assert(resp8.body.find("\"resources\":[]") != std::string::npos);

    // 9. Arena scratchpad offset > 0 verification
    assert(arena.get_offset() > 0);

    // 10. HEAD /api/v1/nodes -> 200 OK with empty body
    HttpRequest r9;
    r9.method = HttpMethod::Head;
    r9.path = "/api/v1/nodes";
    HttpResponse resp9 = handler.handle_request(r9, arena);
    assert(resp9.status_code == 200);
    assert(resp9.body.empty());

    // 11. HEAD /api/v1/pods -> 200 OK with empty body
    HttpRequest r10;
    r10.method = HttpMethod::Head;
    r10.path = "/api/v1/pods";
    HttpResponse resp10 = handler.handle_request(r10, arena);
    assert(resp10.status_code == 200);
    assert(resp10.body.empty());

    // 12. HEAD /apis/apps/v1/namespaces/default/deployments -> 200 OK with empty body
    HttpRequest r11;
    r11.method = HttpMethod::Head;
    r11.path = "/apis/apps/v1/namespaces/default/deployments";
    HttpResponse resp11 = handler.handle_request(r11, arena);
    assert(resp11.status_code == 200);
    assert(resp11.body.empty());

    std::cout << "[PASS] test_discovery_and_method_enforcement" << std::endl;
}

void test_chunked_stream_writer_framing() {
    std::cout << "[RUN] test_chunked_stream_writer_framing" << std::endl;
    TestBufferSink sink;
    {
        ChunkedStreamWriter writer(sink);
        assert(writer.send_headers("application/json", 200));

        // Write 600 bytes to force at least one 512-byte chunk flush
        std::string payload(600, 'X');
        assert(writer.write(payload));
        assert(writer.finish());
    }

    // Verify headers
    assert(sink.data.find("HTTP/1.1 200 OK\r\n") != std::string::npos);
    assert(sink.data.find("Transfer-Encoding: chunked\r\n") != std::string::npos);

    // Verify terminal chunk
    assert(sink.data.rfind("0\r\n\r\n") != std::string::npos);

    std::cout << "[PASS] test_chunked_stream_writer_framing" << std::endl;
}

void test_table_serialization_columns() {
    std::cout << "[RUN] test_table_serialization_columns" << std::endl;
    std::vector<std::string> node_jsons = {
        "{\"kind\":\"Node\",\"metadata\":{\"name\":\"esp32-node-01\"},"
        "\"status\":{\"conditions\":[{\"type\":\"Ready\",\"status\":\"True\"}],"
        "\"addresses\":[{\"type\":\"InternalIP\",\"address\":\"192.168.4.1\"}],"
        "\"nodeInfo\":{\"kubeletVersion\":\"v1.33.0-kube32\",\"osImage\":\"ESP-IDF v5.2\"}}}"
    };

    std::string table_json = TableSerializer::build_nodes_table_json(node_jsons);
    assert(table_json.find("\"kind\":\"Table\"") != std::string::npos);
    assert(table_json.find("\"name\":\"NAME\"") != std::string::npos);
    assert(table_json.find("\"name\":\"STATUS\"") != std::string::npos);
    assert(table_json.find("\"name\":\"INTERNAL-IP\"") != std::string::npos);
    assert(table_json.find("\"name\":\"OS-IMAGE\"") != std::string::npos);
    assert(table_json.find("\"esp32-node-01\"") != std::string::npos);
    assert(table_json.find("\"Ready\"") != std::string::npos);
    assert(table_json.find("\"192.168.4.1\"") != std::string::npos);

    std::cout << "[PASS] test_table_serialization_columns" << std::endl;
}

void test_json_recursion_depth_limit() {
    std::cout << "[RUN] test_json_recursion_depth_limit" << std::endl;

    // Shallow depth (20) should parse fine
    std::string shallow;
    for (int i = 0; i < 20; ++i) shallow += "{\"a\":";
    shallow += "1";
    for (int i = 0; i < 20; ++i) shallow += "}";

    kube32::models::JsonValue doc;
    std::string err;
    assert(kube32::models::parse_json(shallow, doc, err));

    // Deep depth (40) should be cleanly rejected with depth exceeded error
    std::string deep;
    for (int i = 0; i < 40; ++i) deep += "{\"a\":";
    deep += "1";
    for (int i = 0; i < 40; ++i) deep += "}";

    assert(!kube32::models::parse_json(deep, doc, err));
    assert(err.find("maximum JSON nesting depth exceeded") != std::string::npos);

    std::cout << "[PASS] test_json_recursion_depth_limit" << std::endl;
}

int main() {
    std::cout << "=== Running Kube32 API Server & Router Unit Tests ===" << std::endl;
    test_uri_normalization();
    test_route_pattern_matching();
    test_discovery_and_method_enforcement();
    test_chunked_stream_writer_framing();
    test_table_serialization_columns();
    test_json_recursion_depth_limit();
    std::cout << "=== All API Server Tests Passed Successfully ===" << std::endl;
    return 0;
}
