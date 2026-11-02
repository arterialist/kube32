# Releases

Each kube32 release `X.Y.Z` publishes prebuilt firmware, simulators, SDKs and a container image, so boards, the simulator and pods need no clone of the repository. Releases are listed on [GitHub](https://github.com/arterialist/kube32/releases), with notes from `CHANGELOG.md`.

## Assets

| Asset | Contents |
| --- | --- |
| `kube32-firmware-esp32s3-X.Y.Z.bin` | For a first install: one image written at `0x0` with the bootloader, partition table, empty `otadata`, and the firmware in the first app slot. It fills the gaps with `0xff` and erases NVS (`0x9000` to `0x10fff`) with them. |
| `kube32-firmware-esp32s3-X.Y.Z-bootloader.bin`, `-partitions.bin`, `-otadata.bin`, `-app.bin` | The same parts one by one, for updates over USB that keep NVS: bootloader at `0x0`, partition table at `0x8000`, empty `otadata` at `0x11000` (the board starts the first app slot), app at `0x20000` |
| `kube32-firmware-esp32s3-X.Y.Z-flash.json` | The parts with their offsets, sizes and SHA-256, and the flash mode, frequency and size (`dio`, `80m`, `8MB`) |
| `manifest.json` | ESP Web Tools manifest for the merged image, used by the [browser installer](flash.md) |
| `kube32-sim-X.Y.Z-<platform>.tar.gz` | `kube32-sim` for `macos-arm64`, `macos-x86_64` (macOS 13 or later), `linux-x86_64` and `linux-arm64` (statically linked) |
| `kube32-c-sdk-X.Y.Z.tar.gz` | `include/runtime/k32_app_abi.h`, a Makefile and an example request pod |

Each image and archive has a `.sha256` file next to it, in the format `sha256sum -c` reads. The firmware reports `X.Y.Z` as its version in `GET /version` (`kube32Version`), `K32_CMD:FIRMWARE` and the node's `osImage`.

The app image is also what [firmware rollouts](firmware-updates.md) install over the network (`kube32-cli firmware push`). With `esptool`, a first install writes the merged image, and an update writes the parts:

```sh
# First install: erases the stored settings
esptool --chip esp32s3 --port <serial-port> --baud 921600 write-flash 0x0 kube32-firmware-esp32s3-X.Y.Z.bin

# Update: keeps NVS
esptool --chip esp32s3 --port <serial-port> --baud 921600 write-flash \
    0x0 kube32-firmware-esp32s3-X.Y.Z-bootloader.bin 0x8000 kube32-firmware-esp32s3-X.Y.Z-partitions.bin \
    0x11000 kube32-firmware-esp32s3-X.Y.Z-otadata.bin 0x20000 kube32-firmware-esp32s3-X.Y.Z-app.bin
```

## Container image

`ghcr.io/arterialist/kube32-sim` runs the simulator on `linux/amd64` and `linux/arm64`, tagged `X.Y.Z` and `latest`. It starts a control plane and one worker with the API server on port 6443, the Ingress controller on port 8080 and the pod HTTP server on port 17000:

```sh
docker run --rm -p 6443:6443 -p 8080:8080 ghcr.io/arterialist/kube32-sim
```

In a second terminal:

```sh
kubectl config set-cluster kube32-sim --server=http://127.0.0.1:6443
kubectl config set-context kube32-sim --cluster=kube32-sim
kubectl config use-context kube32-sim
kubectl get nodes
kubectl apply -f https://raw.githubusercontent.com/arterialist/kube32/main/examples/echo.yaml
curl http://127.0.0.1:8080/echo
```

Arguments after the image name replace the default `--ingress-port 8080 --pod-port 17000`; `--help` lists them all.

## SDKs

The Rust SDK is the [`kube32-sdk`](https://crates.io/crates/kube32-sdk) crate, versioned with the release:

```sh
cargo add kube32-sdk
```

The C SDK archive builds its example with `make` (clang with the `wasm32` target and `wasm-ld` or `rust-lld`). [Writing pods](app-development.md) covers both.

## Cutting a release

1. Move the `[Unreleased]` entries of `CHANGELOG.md` under a heading `## [X.Y.Z] - YYYY-MM-DD` and merge that to `main`.
2. Tag the commit and push the tag:

    ```sh
    git tag -a vX.Y.Z -m "kube32 X.Y.Z"
    git push origin vX.Y.Z
    ```

The tag starts `.github/workflows/release.yml`:

| Job | Work |
| --- | --- |
| Version and release notes | Takes `X.Y.Z` from the tag and the notes from the `CHANGELOG.md` section, and fails when the section is missing |
| Firmware | `pio run -e esp32s3` with `KUBE32_VERSION=X.Y.Z`, then `scripts/release/package-firmware.py`: merged image, parts, `flash.json`, manifest, checksums |
| Simulator | `kube32-sim` for each platform: macOS builds against a static OpenSSL (`scripts/release/build-openssl-macos.sh`), Linux builds are the Dockerfile's static musl build on native runners. Each binary starts and serves the API as `X.Y.Z` (`scripts/release/smoke-sim.sh`) |
| C SDK | `scripts/release/package-c-sdk.sh`, then builds the example from the archive |
| Container image | Puts the Linux binaries into the image for both platforms, checks it, and pushes `X.Y.Z` and `latest` |
| Rust crate | Sets the crate version to `X.Y.Z` and runs `cargo publish` |
| GitHub Release | Checks every checksum, creates release `vX.Y.Z` with the assets, and starts the docs workflow, which copies the merged image and manifest to the site for the browser installer |

Nothing is published until the firmware, simulator and C SDK jobs succeed. A tag with a suffix, such as `v0.5.0-rc.1`, makes a prerelease: no `latest` image tag, and the browser installer keeps the previous release.

Run the workflow by hand from the Actions tab to rehearse a release. With **dry-run** on (the default), it builds everything as version `0.0.0-dev` (or the version given) and keeps the assets as the workflow artifact `release-dry-run`. Run on a tag with **dry-run** off, it publishes that tag's release again: it replaces the release's assets, and skips the crate when crates.io has that version.

### Repository settings

| Setting | Value |
| --- | --- |
| Settings, Actions, General, Workflow permissions | Read repository contents. The jobs ask for `contents: write`, `packages: write`, `actions: write` and `id-token: write` themselves. |
| Settings, Pages, Source | GitHub Actions |
| Secret `CARGO_REGISTRY_TOKEN` | A crates.io API token with the `publish-new` and `publish-update` scopes, for the first publish of `kube32-sdk` |
| crates.io trusted publishing | After the first publish: on crates.io, `kube32-sdk`, Settings, Trusted Publishing, add repository `arterialist/kube32` and workflow `release.yml`, then delete the secret. The crate job asks crates.io for a token when the secret is absent. |
| Package `kube32-sim` | After the first push, set the package's visibility to public under the repository's Packages, and keep "Inherit access from source repository" |
