#pragma once

// The firmware image a rollout distributes, staged in the control plane's
// passive app slot (hal/firmware_slots.hpp), on the API server:
//
//   GET /apis/kube32.io/v1/firmware
//       the staged image as a FirmwareImage object: phase (Empty, Receiving,
//       Staged, Failed), sha256, size, bytes received, version, slot
//   PUT /apis/kube32.io/v1/firmware/image?offset=<n>&size=<bytes>&sha256=<hex>
//       bytes [offset, offset + body length) of the image. Offset 0 starts
//       an upload and erases the slot; every further range continues where
//       the last one ended (`received` in the answer), so an upload that
//       broke off resumes there. The range that completes the image has its
//       SHA-256 checked and the image verified; the image is then Staged.
//       Refused while a FirmwareRollout is in progress.
//   GET /apis/kube32.io/v1/firmware/image?sha256=<hex>&offset=<n>&length=<n>
//       bytes of the staged image, at most 256 KiB per request (kubelets
//       download it range by range)
//
// Bodies stream through the connection's buffer into flash: an upload takes
// no memory beyond it, and a range may be any size (the serial console
// gateway sends ranges below 60 KiB, a client on Wi-Fi may send the whole
// image at once). Requests are authenticated like every API call. Other
// requests go to `next` (the node proxy).
//
// What is staged survives a restart of the control plane (notes kept with
// the slots); once the control plane runs the staged image itself, nothing is
// staged any more.

#include "api/http_server.hpp"
#include "hal/firmware_slots.hpp"
#include "osal/mutex.hpp"
#include "osal/sha256.hpp"
#include "storage/store.hpp"

#include <string>

namespace kube32::api {

struct StagedFirmware {
    std::string phase{"Empty"};   // Empty, Receiving, Staged, Failed
    std::string sha256;           // of the whole image file
    size_t size{0};
    size_t received{0};
    std::string version;          // read from the image once it is staged
    std::string message;          // why the last upload failed
};

class FirmwareApi : public HttpHandler {
public:
    FirmwareApi(hal::FirmwareSlots& slots, storage::Store& store, RouteHandler& routes, HttpHandler* next = nullptr);

    bool handle(HttpConnection& conn) override;
    bool streams_body(const HttpRequest& req) override;

    StagedFirmware staged() const;
    std::string status_json() const;

    // Largest range a GET returns
    static constexpr size_t kMaxRange = 256 * 1024;

private:
    bool upload(HttpConnection& conn);
    bool download(HttpConnection& conn);
    bool reply(HttpConnection& conn, int code, const std::string& body);
    bool fail(HttpConnection& conn, int code, const char* reason, const std::string& message);
    // A FirmwareRollout that has not completed or failed
    bool rollout_active(std::string& name);
    void store_notes(const StagedFirmware& st);

    hal::FirmwareSlots& slots_;
    storage::Store& store_;
    RouteHandler& routes_;
    HttpHandler* next_;
    mutable osal::Mutex mu_;
    StagedFirmware st_;         // guarded by mu_
    bool busy_{false};          // a range is being written (guarded by mu_)
    osal::Sha256 sha_;          // of the bytes received so far (the writer only)
};

} // namespace kube32::api
