# microSD card: images and pod storage

The M5Stack Cardputer (the display board, `esp32-node-02`) has a microSD
slot. Kube32 uses it for two things: WebAssembly images that are not
compiled into the firmware, and persistent storage for pods.

Everything on this page runs on the board; the host simulator provides the
same behaviour over a directory so the tests exercise the same code.

## Hardware and mount

| Item | Value |
| --- | --- |
| Bus | SPI3_HOST, dedicated to the card (the LCD is on SPI2_HOST) |
| Pins | MOSI GPIO 14, MISO GPIO 39, SCK GPIO 40, CS GPIO 12 |
| Clock | 20 MHz |
| Filesystem | FAT (FAT32 as formatted by the user), ESP-IDF `esp_driver_sdspi` + `fatfs`, 512-byte sectors, long file names |
| Mount point | `/sd` |
| Open files | 4 node-wide (`max_files`) |
| Formatting | never; a card without a FAT filesystem is reported, not formatted |

The card is mounted at boot on boards with the `cardputer` profile
(`K32_CMD:BOARD:cardputer`), right after the display. Since commit `f54968a`
the boot log first prints `[SD] mount cost <n> bytes of internal RAM`, then
one line with the card's figures:

```
[SD] SDHC/SDXC SD32G mounted at /sd: 30436 MB total, 30412 MB free, SPI3 20000 kHz
```

or the reason it did not mount:

```
[SD] not mounted: no card answered on SPI3 (ESP_ERR_TIMEOUT)
[SD] not mounted: card found but no FAT filesystem on it (not formatting; format it FAT32 on a PC)
```

### Console

`K32_CMD:SD` prints the mount state, the figures, and what is under
`/sd/kube32` (images and volumes, up to 64 entries), then `K32_OK sd`:

```
K32_SD mounted=1 name=SD32G type=SDHC/SDXC total_bytes=31914983424 free_bytes=31889817600 spi_khz=20000 path=/sd
K32_SD /sd/kube32/images/ dir
K32_SD /sd/kube32/images/sdnote.wasm 4815
K32_SD /sd/kube32/volumes/ dir
K32_SD /sd/kube32/volumes/notes/ dir
K32_SD /sd/kube32/volumes/notes/boot.log 29
K32_OK sd
```

### Kubernetes

The node advertises the card as an extended resource:

```
kubectl --context kube32 get node esp32-node-02 -o jsonpath='{.status.capacity.hardware\.kube32\.io/sd} {.status.allocatable.hardware\.kube32\.io/sd}'
```

`capacity` is the card's size, `allocatable` the free space, both measured
at every heartbeat and absent on a node without a card.

## Card layout

```
/sd/kube32/
  images/<name>.wasm            wasm://sd/<name>
  images/<name>-<tag>.wasm      wasm://sd/<name>:<tag>
  volumes/<claim>/              one directory per volume claim
```

## Images from the card

An image reference `wasm://sd/<name>` or `wasm://sd/<name>:<tag>` makes the
kubelet pull `/sd/kube32/images/<name>.wasm` (or `<name>-<tag>.wasm`) into
a heap buffer sized from the file, 4 KiB at a time. The runtime takes that
buffer over for the module's lifetime (wasm3 keeps pointers into the
module), so the image exists once in RAM. The embedded registry
(`wasm://embedded/...`) and the `kube32.io/wasm-base64` annotation keep
working as before.

Limits: 256 KiB per image on a board without PSRAM, 2 MiB with PSRAM. Names
and tags are `[A-Za-z0-9._-]`. Failures are pod events and `ErrImagePull`
with the reason: file not found, over the limit, no card, allocation failed.

```
$ kubectl --context kube32 get events --field-selector involvedObject.name=sdnote
Pulling   Pulling image "wasm://sd/sdnote" from the sd card
Pulled    Successfully pulled image "wasm://sd/sdnote" in 9ms (4815 bytes, sd card, /sd/kube32/images/sdnote.wasm)
Created   Created container sdnote
Started   Started container sdnote
```

## Pod storage

### Manifest shape

A pod gets a directory on the card when it requests the resource and names a
volume claim. Two volume spellings are accepted, `persistentVolumeClaim` and
the explicit `kube32.io/sd`. Both go through a plain `kubectl apply`: the API
server checks only top-level and `metadata` fields, so the non-standard
volume key is not rejected. Both spellings were applied on the host
simulator for this page:

```yaml
spec:
  volumes:
    - name: data
      persistentVolumeClaim:
        claimName: notes            # -> /sd/kube32/volumes/notes/
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

Rules enforced at admission by the capability firewall:

* `hardware.kube32.io/sd` without a volume claim is rejected
  (`CapabilityAdmissionFailed`).
* A claim is a directory name: 1..32 characters of `a-z`, `0-9`, `-`, no
  leading `-`. One claim per pod. Two pods may share a claim.
* A volume without the resource request grants nothing: every `k32_fs_*`
  call returns `K32_ERR_PERM` and is logged as a firewall violation.
* A pod with the grant on a node without a card fails at admission with
  "no sd card is mounted on this node".

The directory is created on first use. Nothing is deleted when the pod goes
away: that is what persistent storage is for. `securityContext.capabilities.add: ["SD"]`
is accepted as an alias of the resource request.

### ABI

Declared in `include/runtime/k32_app_abi.h`. All paths are relative to the
pod's volume; absolute paths, `..`, `.`, empty components and characters
outside `[A-Za-z0-9._-]` are rejected with `K32_ERR_INVAL`; paths over
96 bytes with `K32_ERR_NAMETOOLONG`.

| Import | Returns |
| --- | --- |
| `k32_fs_open(path, path_len, flags)` | fd `0..3`, or error. Flags: `K32_FS_O_RDONLY/WRONLY/RDWR`, `K32_FS_O_CREAT`, `K32_FS_O_EXCL`, `K32_FS_O_TRUNC`, `K32_FS_O_APPEND` |
| `k32_fs_read(fd, buf, len)` | bytes read (at most 4096 per call), `0` at end of file |
| `k32_fs_write(fd, buf, len)` | bytes written (at most 4096 per call) |
| `k32_fs_close(fd)` | `0` |
| `k32_fs_size(fd)` | file size in bytes |
| `k32_fs_list(dir, dir_len, out, out_cap)` | bytes written to `out`: one `name\n` per entry, directories as `name/\n`; `dir` empty lists the volume root; entries that do not fit are left out whole |
| `k32_fs_remove(path, path_len)` | `0`; removes a file or an empty directory |

Errors are negative errno values: `K32_ERR_PERM` (-1), `K32_ERR_NOENT` (-2),
`K32_ERR_IO` (-5), `K32_ERR_BADF` (-9), `K32_ERR_FAULT` (-14),
`K32_ERR_EXIST` (-17), `K32_ERR_NODEV` (-19), `K32_ERR_ISDIR` (-21),
`K32_ERR_INVAL` (-22), `K32_ERR_MFILE` (-24), `K32_ERR_NOSPC` (-28),
`K32_ERR_NAMETOOLONG` (-36).

A pod holds at most 4 descriptors; the node has 4 in total across pods and
image pulls, so a fifth open anywhere returns `K32_ERR_MFILE`. Descriptors
are closed by the host when the container stops or restarts. The imports
are linked into every module; a module that does not import them is
unaffected, and a module that imports them without the grant still
instantiates.

### Example: sdnote

`apps/sdnote/sdnote.c` appends `boot at uptime_ms=<n>` to `boot.log` in its
volume on `k32_init` (and fails the start if it cannot: an honest signal
that the storage is missing), serves `GET /log` with the file, `GET /ls`
with the directory listing, and `DELETE /log`.

```
make -C apps/sdnote                      # -> images/sd/sdnote.wasm (not embedded in the firmware)
cp images/sd/sdnote.wasm /Volumes/<card>/kube32/images/sdnote.wasm   # mkdir -p kube32/images first
kubectl --context kube32 apply -f examples/sdnote.yaml
kubectl --context kube32 logs sdnote     # sdnote: appended to boot.log, bytes=29
```

`GET /log` cannot be reached on the Cardputer. Since commit `f54968a`,
boards without PSRAM no longer start the pod HTTP server on port 7000: its
16 KB could not be spared next to the HUD pod. The log line above and
`K32_CMD:SD`, which lists `boot.log` with its size, show what the pod
wrote. On the host simulator the pod HTTP server runs; there,
`X-Kube32-Pod: default/sdnote` selects the pod when several handle
requests.

## Host simulator

Set `KUBE32_SD_ROOT=/some/directory` before starting `kube32-sim` and that
directory is the card: `[SD] simulated card (directory) ...` is printed with
figures from `statvfs()`, `wasm://sd/<name>` reads
`$KUBE32_SD_ROOT/kube32/images/<name>.wasm`, and volumes live under
`$KUBE32_SD_ROOT/kube32/volumes/`. Without the variable the host node has
no card, like a board without one. `tests/test_sd_card.cpp` runs the HAL,
the firewall rules, the ABI (through `PodFs` and through the real
`sdnote.wasm` guest) and the kubelet pull path against a temporary
directory.

## Memory on a board without PSRAM

Steady-state cost with the card mounted, from the allocations involved
(measured figures appear in the `[MEM]` lines on the console):

| Allocation | Bytes |
| --- | --- |
| `sdmmc_card_t` + sdspi slot | about 0.6 KB |
| FATFS volume object (512-byte window) | about 0.6 KB |
| VFS FAT context with 4 `FIL` slots (512-byte cache each) | about 2.6 KB |
| SPI3 bus DMA descriptors | about 0.3 KB |
| per pod: fd table | 40 bytes |
| image pull: the module itself, once, for its lifetime | file size |
| transient per `k32_fs_*` call: LFN buffer on the heap | 0.5 KB |

Nothing is buffered beyond one 4 KiB chunk; `k32_fs_read`/`k32_fs_write`
move data straight between guest memory and the FATFS file cache.
