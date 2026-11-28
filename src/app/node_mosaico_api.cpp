#include "app/node_mosaico_api.hpp"
#include "hal/board.hpp"
#include "hal/mosaico_modules.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace kube32::app {

bool is_node_mosaico_path(std::string_view path) {
    return path == "/modules" || path == "/camera/snapshot" || path == "/camera/preview";
}

namespace {

bool authorized(const api::HttpRequest& req, const std::string& token) {
    if (token.empty()) return true;
    static constexpr std::string_view kBearer = "Bearer ";
    if (req.authorization.rfind(kBearer, 0) != 0) return false;
    return req.authorization.substr(kBearer.size()) == token;
}

std::string json_escape(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        if (c == '"' || c == '\\') out.push_back('\\');
        if (static_cast<unsigned char>(c) >= 0x20) out.push_back(c);
    }
    return out;
}

uint8_t parse_quality(std::string_view query) {
    size_t pos = query.find("quality=");
    if (pos == std::string_view::npos) return 75;
    pos += 8;
    int val = 0;
    bool any = false;
    while (pos < query.size() && query[pos] >= '0' && query[pos] <= '9') {
        val = val * 10 + (query[pos++] - '0');
        any = true;
        if (val > 100) break;
    }
    if (!any) return 75;
    if (val < 10) return 10;
    if (val > 95) return 95;
    return static_cast<uint8_t>(val);
}

std::string slot_json(const char* name, const hal::MosaicoSlotStatus& s) {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "{\"slot\":\"%s\",\"present\":%s,\"valid\":%s,\"eeprom\":\"0x%02x\",\"type\":\"%s\",\"boardType\":%u,\"boardId\":%u,\"hwVersion\":%u,\"name\":\"%s\"}",
                  name, s.present ? "true" : "false", s.descriptor_valid ? "true" : "false",
                  s.eeprom_addr, s.type_name ? s.type_name : "none",
                  static_cast<unsigned>(s.board_type), static_cast<unsigned>(s.board_id),
                  static_cast<unsigned>(s.hw_version), json_escape(s.board_name).c_str());
    return buf;
}

bool serve_modules_status(api::HttpConnection& conn) {
    auto& mods = hal::MosaicoModules::instance();
    std::string body = "{\"slots\":[";
    body += slot_json("left", mods.slot_status(0));
    body += ",";
    body += slot_json("right", mods.slot_status(1));
    body += "],\"camera\":{";
    hal::MosaicoCameraInfo cinfo{};
    if (mods.camera_info(cinfo) == hal::HAL_OK && cinfo.present) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "\"present\":true,\"width\":%u,\"height\":%u,\"sensor\":\"%s\"",
                      (unsigned)cinfo.width, (unsigned)cinfo.height, json_escape(cinfo.sensor).c_str());
        body += buf;
    } else {
        body += "\"present\":false";
    }
    body += "},\"interact\":{";
    hal::MosaicoInteractInputs in{};
    if (mods.interact_read(in) == hal::HAL_OK) {
        char buf[224];
        std::snprintf(buf, sizeof(buf),
                      "\"present\":true,\"left_pressed\":%s,\"right_pressed\":%s,\"motion\":%s,\"light_pct\":%u,\"light_raw\":%d,\"led_count\":%u,\"button_mode\":\"%s\"",
                      in.left_pressed ? "true" : "false",
                      in.right_pressed ? "true" : "false",
                      in.motion_detected ? "true" : "false",
                      (unsigned)in.light_level, (int)in.light_raw, (unsigned)in.led_count,
                      in.button_mode == 2 ? "touch" : "gpio");
        body += buf;
    } else {
        body += "\"present\":false";
    }
    body += "}}\n";
    return conn.send_simple(200, "application/json", body);
}

bool serve_camera_snapshot(api::HttpConnection& conn) {
    auto& mods = hal::MosaicoModules::instance();
    if (!mods.has_camera()) {
        return conn.send_simple(503, "application/json", "{\"ok\":false,\"error\":\"camera module not attached\"}\n");
    }
    const uint8_t quality = parse_quality(conn.request().query_string);
    std::vector<uint8_t> jpeg;
    hal::MosaicoCameraInfo cinfo{};
    const int32_t rc = mods.camera_capture_jpeg(jpeg, &cinfo, quality);
    if (rc != hal::HAL_OK || jpeg.empty()) {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "{\"ok\":false,\"error\":\"camera capture failed\",\"code\":%d}\n", (int)rc);
        return conn.send_simple(503, "application/json", msg);
    }
    char hdr[160];
    std::snprintf(hdr, sizeof(hdr),
                  "Cache-Control: no-store\r\nX-Camera-Width: %u\r\nX-Camera-Height: %u\r\n",
                  (unsigned)cinfo.width, (unsigned)cinfo.height);
    if (!conn.begin_fixed(200, "image/jpeg", jpeg.size(), hdr)) return false;
    if (conn.request().method == api::HttpMethod::Head) return true;
    return conn.send_raw(jpeg.data(), jpeg.size());
}

bool serve_camera_preview(api::HttpConnection& conn) {
    auto& mods = hal::MosaicoModules::instance();
    if (!mods.has_camera()) {
        return conn.send_simple(503, "application/json", "{\"ok\":false,\"error\":\"camera module not attached\"}\n");
    }
    hal::Board* board = hal::board_for("esp-mosaico");
    if (!board) {
        return conn.send_simple(503, "application/json", "{\"ok\":false,\"error\":\"display unavailable\"}\n");
    }
    const int32_t rc = mods.camera_preview_to_display(*board);
    if (rc != hal::HAL_OK) {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "{\"ok\":false,\"error\":\"camera preview failed\",\"code\":%d}\n", (int)rc);
        return conn.send_simple(503, "application/json", msg);
    }
    hal::MosaicoCameraInfo cinfo{};
    (void)mods.camera_info(cinfo);
    char ok[128];
    std::snprintf(ok, sizeof(ok), "{\"ok\":true,\"width\":%u,\"height\":%u}\n",
                  (unsigned)cinfo.width, (unsigned)cinfo.height);
    return conn.send_simple(200, "application/json", ok);
}

}  // namespace

bool serve_node_mosaico(api::HttpConnection& conn, std::string_view path, const std::string& token) {
    const api::HttpRequest& req = conn.request();
    if (!authorized(req, token)) {
        return conn.send_simple(401, "application/json", "{\"error\":\"unauthorized\"}\n");
    }
    if (path == "/modules") return serve_modules_status(conn);
    if (path == "/camera/snapshot") return serve_camera_snapshot(conn);
    if (path == "/camera/preview") return serve_camera_preview(conn);
    return false;
}

}  // namespace kube32::app
