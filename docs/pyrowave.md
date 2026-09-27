# Native PyroWave on Linux

This optional backend encodes DMA-BUF RGB capture directly on the selected Vulkan capture adapter. SDR uses Rec.709; HDR requires PQ/BT.2020 and at least 10-bit RGB capture. Both 4:2:0 and 4:4:4 use PyroWave's native metadata and 16-bit UNORM output planes. Physical HDR display output and AMD modifier handling still require hardware verification.

## Build the patched dependency

Use the PyroWave checkout delivered with this patch. Its API version is **0.7.0 (local patch, not an upstream release)**, based on `89f7e47d4abbf650c91fae766728af866c5e32a0`. Granite is pinned to `1b2d1801d2910fb09ebcded2f0bb3a3a781103b5` by PyroWave's `checkout_granite.sh`. Do not substitute upstream 0.6 or an arbitrary newer API.

```sh
export PW_ROOT=/home/karsy/Projects/Pyrowave
export PW_PREFIX="$PW_ROOT/build/pyrowave-install"
cmake -S "$PW_ROOT/pyrowave" -B "$PW_ROOT/build/pyrowave" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PW_PREFIX" \
  -DPYROWAVE_DEVEL=OFF -DPYROWAVE_UTILS=OFF
cmake --build "$PW_ROOT/build/pyrowave" -j4
cmake --install "$PW_ROOT/build/pyrowave"
export PKG_CONFIG_PATH="$PW_PREFIX/share/pkgconfig:$PKG_CONFIG_PATH"
export LD_LIBRARY_PATH="$PW_PREFIX/lib:$LD_LIBRARY_PATH"
```

Both applications use the installed standalone shared C API through pkg-config. Neither downloads PyroWave during configuration. Their patched moonlight-common-c checkouts must accompany the application patch.

## Build and configure Sunshine

```sh
cmake -S "$PW_ROOT/Sunshine" -B "$PW_ROOT/Sunshine/cmake-build-pyrowave" \
  -DSUNSHINE_ENABLE_PYROWAVE=ON -DBUILD_TESTS=ON
cmake --build "$PW_ROOT/Sunshine/cmake-build-pyrowave" -j4
```

Install the normal Sunshine dependencies described in the main build documentation. `SUNSHINE_ENABLE_PYROWAVE` defaults to OFF. The Linux build rejects missing/incompatible PyroWave rather than silently disabling it.

Set `pyrowave_enabled = enabled`. `pyrowave_bitrate = 0` accepts the client's request; a positive value caps it in kbps (up to 2,000,000). The normal `max_bitrate` also applies. The Web UI exposes these fields only in compiled builds. The client selects 4:4:4 and HDR using its existing preferences.

Use DMA-BUF capture (KMS, KWin, or portal) with the VAAPI/Vulkan capture path. CPU/X11 capture, flipped buffers, scRGB/FP16 capture and independent plane allocations are rejected. HDR capture must be enabled on the source display. HDR-to-SDR tone mapping is not implemented. Cursor metadata compositing and aspect-ratio letterboxing are follow-up work; use matching source/client aspect ratios.

## Native import test

```sh
mkdir -p "$PW_ROOT/build/native-frames"
SUNSHINE_TEST_PYROWAVE_GPU=1 \
SUNSHINE_TEST_PYROWAVE_OUTPUT="$PW_ROOT/build/native-frames" \
  "$PW_ROOT/Sunshine/cmake-build-pyrowave/tests/test_sunshine" \
  --gtest_filter='PyroWave.*'
```

The opt-in test creates real GBM DMA-BUFs, encodes repeated RGB8/RGB10 frames through the production importer, and exports all SDR/HDR and 420/444 combinations for the Moonlight renderer test. It needs access to a Vulkan render device. It is not a compositor capture test.

Debug logging reports capture age, encode/readback time, encoded bytes, effective bitrate and GPU timestamp statistics. Pixel data remains on the GPU; encoded coefficient bytes are read back for transport.

For the provided Arch/CachyOS test archive, see [installation and rollback](pyrowave-install.md).
