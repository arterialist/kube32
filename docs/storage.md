# Storage and volumes

Each node mounts one persistent storage root at boot. Kube32 uses this filesystem for the control plane object store, pod volumes, container logs, and microSD WebAssembly images.

## Node storage root and layout

A node mounts `/sd` when its board profile has a microSD slot (`K32_CMD:BOARD:cardputer`) and a FAT32 card is present. Otherwise, it mounts the internal flash `storage` partition at `/data` using FAT with wear levelling, formatting the partition on first use. Only one volume is mounted per board.

On the host simulator, `KUBE32_SD_ROOT=/some/directory` mounts a directory as `/sd`. When `KUBE32_SD_ROOT` is unset, `kube32-sim` uses `--data-dir` as the internal flash root.

```
<root>/kube32/
  store/journal.img             object store journal (control plane)
  volumes/<claim>/              one directory per volume claim
  logs/<ns>_<pod>/<container>.log
  images/<name>.wasm            wasm://sd/<name> (card only)
  images/<name>-<tag>.wasm      wasm://sd/<name>:<tag> (card only)
  images/<name>/                k32_resp_asset bundle (card only)
```

### Partition table (8 MB flash)

| Name | Type | Subtype | Offset | Size |
| --- | --- | --- | --- | --- |
| `nvs` | `data` | `nvs` | `0x9000` | 32 KB (`0x8000`) |
| `phy_init` | `data` | `phy` | `0x11000` | 4 KB (`0x1000`) |
| `factory` | `app` | `factory` | `0x20000` | 3 MB (`0x300000`) |
| `storage` | `data` | `fat` | `0x320000` | 3.5 MB (`0x380000`) |
| `reserved` | `data` | `0xff` | `0x6A0000` | 1.375 MB (`0x160000`) |

### microSD hardware and mount

| Item | Value |
| --- | --- |
| Bus | SPI3_HOST |
| Pins | MOSI GPIO 14, MISO GPIO 39, SCK GPIO 40, CS GPIO 12 |
| Clock | 20 MHz |
| Filesystem | FAT (FAT32), ESP-IDF `esp_driver_sdspi` + `fatfs`, 512-byte sectors, long file names |
| Mount point | `/sd` |
| Open files | 4 node-wide (`max_files`) |
| Formatting | Never. You must format the card as FAT32 before insertion. |

Send `K32_CMD:SD` over the serial console to inspect the card state and list up to 64 entries under `/sd/kube32`:

```
K32_SD mounted=1 name=SD32G type=SDHC/SDXC total_bytes=31914983424 free_bytes=31889817600 spi_khz=20000 path=/sd
K32_SD /sd/kube32/images/ dir
K32_SD /sd/kube32/images/sdnote.wasm 4815
K32_SD /sd/kube32/volumes/ dir
K32_SD /sd/kube32/volumes/notes/ dir
K32_SD /sd/kube32/volumes/notes/boot.log 29
K32_OK sd
```

Nodes with a mounted card report total size as `capacity` and free space as `allocatable` under the `hardware.kube32.io/sd` extended resource at each heartbeat:

```
kubectl --context kube32 get node esp32-node-02 -o jsonpath='{.status.capacity.hardware\.kube32\.io/sd} {.status.allocatable.hardware\.kube32\.io/sd}'
```

## Control plane object store

The control plane persists cluster objects in a single log-structured journal file at `<root>/kube32/store/journal.img` (enabled on the simulator with `--enable-flash`). Records are CRC-checked and power-cut safe.

* Journal size is 1 MiB on internal flash (~480 KB of live objects) and 4 MiB on a microSD card.
* When the journal is full, any persisted create or update that cannot fit returns `500 InternalError` (`object store is full: X of Y bytes in use, N more needed`). Shrinking updates and deletes continue to work.
* Leases and Events stay in RAM.
* Node heartbeat status writes at most every 10 minutes. Changes to Node labels, annotations, spec, or the `Ready` condition write within 1 s.
* All other objects write within 1 s. Unchanged content is skipped.
* `resourceVersion` never goes backwards across restarts. Watch streams opened before a restart receive `410 Gone` and relist. Pods already in `Succeeded` or `Failed` phase do not restart after boot.
* Send `K32_CMD:STORE` over the serial console to inspect journal usage and counters.

## Images from the card

Use `wasm://sd/<name>` or `wasm://sd/<name>:<tag>` to load `/sd/kube32/images/<name>.wasm` or `<name>-<tag>.wasm`. The kubelet reads the file in 4 KiB chunks into a single heap buffer retained for the module's lifetime.

* Maximum image size is 256 KiB without PSRAM, or 2 MiB with PSRAM.
* Names and tags allow `[A-Za-z0-9._-]`.
* Pull failures emit `ErrImagePull` pod events when the file is missing, exceeds the size limit, lacks a mounted card, or fails memory allocation.

```
$ kubectl --context kube32 get events --field-selector involvedObject.name=sdnote
Pulling   Pulling image "wasm://sd/sdnote" from the sd card
Pulled    Successfully pulled image "wasm://sd/sdnote" in 9ms (4815 bytes, sd card, /sd/kube32/images/sdnote.wasm)
Created   Created container sdnote
Started   Started container sdnote
```

## Pod volumes

Declare a volume with `persistentVolumeClaim` to mount `<root>/kube32/volumes/<claim>/` on the node's storage root (microSD card or internal flash). To require a microSD card, use the `kube32.io/sd` volume type or request `hardware.kube32.io/sd: "1"` (or add `securityContext.capabilities.add: ["SD"]`):

```yaml
spec:
  volumes:
    - name: data
      persistentVolumeClaim:
        claimName: notes            # -> <root>/kube32/volumes/notes/
    # or:
    # - name: data
    #   kube32.io/sd: { claim: notes }
  containers:
    - name: sdnote
      image: wasm://sd/sdnote
      resources:
        requests:
          hardware.kube32.io/sd: "1"
```

The capability firewall enforces these admission rules:

* Claim names must be 1 to 32 characters of `a-z`, `0-9`, and `-`, with no leading `-`. A pod can declare one claim, and multiple pods can share a claim.
* Requesting `hardware.kube32.io/sd` without a volume claim fails with `CapabilityAdmissionFailed`.
* Without a volume claim, every `k32_fs_*` call returns `K32_ERR_PERM` and logs a firewall violation.
* Scheduling a pod with `kube32.io/sd` or `hardware.kube32.io/sd` onto a node without a mounted card fails admission.

The runtime creates the volume directory on first use and retains it when the pod terminates.

### Filesystem ABI

`include/runtime/k32_app_abi.h` declares the filesystem host imports. All paths are relative to the pod volume. The runtime rejects absolute paths, `..`, `.`, empty components, and characters outside `[A-Za-z0-9._-]` with `K32_ERR_INVAL`. Paths over 96 bytes fail with `K32_ERR_NAMETOOLONG`.

| Import | Returns |
| --- | --- |
| `k32_fs_open(path, path_len, flags)` | fd `0..3`, or error. Flags: `K32_FS_O_RDONLY/WRONLY/RDWR`, `K32_FS_O_CREAT`, `K32_FS_O_EXCL`, `K32_FS_O_TRUNC`, `K32_FS_O_APPEND` |
| `k32_fs_read(fd, buf, len)` | bytes read, up to 4096 per call, or `0` at EOF |
| `k32_fs_write(fd, buf, len)` | bytes written, up to 4096 per call |
| `k32_fs_close(fd)` | `0` |
| `k32_fs_size(fd)` | file size in bytes |
| `k32_fs_list(dir, dir_len, out, out_cap)` | bytes written to `out` as `name\n` for files and `name/\n` for directories. An empty `dir` lists the volume root. Entries that do not fit are omitted. |
| `k32_fs_remove(path, path_len)` | `0`. Removes a file or an empty directory. |

Errors return negative errno values: `K32_ERR_PERM` (-1), `K32_ERR_NOENT` (-2), `K32_ERR_IO` (-5), `K32_ERR_BADF` (-9), `K32_ERR_NOMEM` (-12), `K32_ERR_FAULT` (-14), `K32_ERR_EXIST` (-17), `K32_ERR_NODEV` (-19), `K32_ERR_ISDIR` (-21), `K32_ERR_INVAL` (-22), `K32_ERR_MFILE` (-24), `K32_ERR_NOSPC` (-28), `K32_ERR_NAMETOOLONG` (-36).

Each pod can open up to 4 file descriptors, subject to the node-wide FAT limit of 4 open files across all pods, logs, and image pulls. Exceeding the limit returns `K32_ERR_MFILE`. The host closes open descriptors when the container stops or restarts.

### Example: sdnote

`apps/sdnote/sdnote.c` appends `boot at uptime_ms=<n>` to `boot.log` during `k32_init`, and serves `GET /log`, `GET /ls`, and `DELETE /log`.

```
make -C apps/sdnote                      # -> images/sd/sdnote.wasm (not embedded in the firmware)
cp images/sd/sdnote.wasm /Volumes/<card>/kube32/images/sdnote.wasm   # mkdir -p kube32/images first
kubectl --context kube32 apply -f examples/sdnote.yaml
kubectl --context kube32 logs sdnote     # sdnote: appended to boot.log, bytes=29
```

On the host simulator, pass the header `X-Kube32-Pod: default/sdnote` to route HTTP requests to the pod on port 7000.

## Container logs

Each node keeps an in-RAM log ring per pod (2 KiB on boards with PSRAM, 1 KiB without PSRAM, 16 KiB on the host simulator) and ships lines to the control plane. When the control plane has storage mounted, it also writes container logs to `<root>/kube32/logs/<namespace>_<pod>/<container>.log`.

* Each log file is capped at 32 KiB. When a file exceeds 32 KiB, the runtime trims it to the newest 16 KiB on a line boundary.
* On container restart, the current log rotates to `<container>.log.1`. `kubectl logs --previous` serves that file.
* Deleting a pod removes its `<namespace>_<pod>` log directory.

## Uploading files

You can copy `.wasm` images to a mounted microSD card on your host with `tools/kube32-cli sd-push <file.wasm> --to /Volumes/<card>` (or over the network with `--node <node>`), or manage files on a running node over the network.

The API server exposes each node's `<root>/kube32/` directory through the node proxy subresource at `/api/v1/nodes/<node>/proxy/storage/<path>`:

* `PUT` accepts `Content-Length` or chunked request bodies (up to 8 MiB by default), writes to `<path>.part`, and renames it into place when complete.
* `GET` streams a file (`application/octet-stream`) or lists a directory as JSON.
* `DELETE` removes a file or an empty directory.

Use `tools/kube32-cli push` to upload a file or directory tree to `<root>/kube32/<path>` via `kubectl replace --raw` (`--path` defaults to `images/`; directories upload recursively and stop at the first error):

```
tools/kube32-cli push <file|dir> --node <node> [--path <path>]
```

## Limitations

* Boards without PSRAM do not run the pod HTTP server on port 7000.
