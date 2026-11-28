/* driver-mosaico-camera: ESP-Mosaico DVP Camera Module device driver.
 *
 * Reports camera presence and sensor resolution via k32_camera_info (granted
 * by hardware.kube32.io/camera). Action "snapshot" triggers a real hardware
 * frame capture + JPEG encode and records the resulting JPEG byte count
 * (full JPEG bytes are served via GET /api/v1/nodes/<node>/proxy/camera/snapshot
 * or from pods with a larger linear memory buffer). */
#define DRIVER "mosaico-camera"
#define INTERVAL_MS 5000
#define ACTIONS 1
#define DESCRIBE "{\"driver\":\"mosaico-camera\",\"readings\":{\"present\":\"bool\",\"width\":\"px\",\"height\":\"px\",\"last_jpeg_bytes\":\"bytes\",\"captures\":\"count\"},\"actions\":[{\"action\":\"snapshot\",\"quality\":\"10-95\"}]}"
#include "../common/driver.h"

static k32_camera_info_t info;
static uint32_t last_jpeg_bytes;
static uint32_t captures;

static void start(void) {
    out_len = 0;
    put(DRIVER ": monitoring left-slot Camera Module every "); put_uint(interval_ms); put(" ms");
    log_out();
}

static int poll(uint32_t now) {
    k32_camera_info_t ci;
    int32_t rc = k32_camera_info(&ci);
    if (rc < 0 || !ci.flags) {
        dev_fail(now, "camera module not attached", rc < 0 ? rc : K32_ERR_NODEV);
        return 0;
    }
    info = ci;
    dev_ok(now);
    publish();
    return 0;
}

static void put_readings(void) {
    key("present"); put_bool(info.flags & 1u);
    key("width"); put_uint(info.width);
    key("height"); put_uint(info.height);
    key("last_jpeg_bytes"); put_uint(last_jpeg_bytes);
    key("captures"); put_uint(captures);
}

static int act(const char* name, uint32_t n, const char* b, const char* end) {
    if (is(name, n, "snapshot")) {
        int32_t q = json_uint(b, end, "quality", 95, 75);
        if (q < 10) { act_error = "quality must be 10 to 95"; return ACT_BAD; }
        int32_t sz = k32_camera_capture_jpeg(0, 0, (uint32_t)q);
        if (sz <= 0) { dev_fail(last_now, "camera snapshot failed", sz); return ACT_FAILED; }
        last_jpeg_bytes = (uint32_t)sz;
        captures++;
        (void)k32_camera_info(&info);
        dev_ok(last_now);
        publish();
        return 0;
    }
    return ACT_BAD;
}
