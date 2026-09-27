# Install the native PyroWave Sunshine test build on Arch/CachyOS

This build includes SDR/HDR and 4:2:0/4:4:4. It is a native x86_64 desktop package built on CachyOS with glibc 2.44; it is not a general Ubuntu/Debian package. The Steam Deck client has its own portable AppImage. GPU drivers remain supplied by your OS.

## Install

Download/copy `Sunshine-PyroWave-Linux-x86_64.tar.gz` and its checksum to your desktop, then:

```sh
sha256sum Sunshine-PyroWave-Linux-x86_64.tar.gz
# Compare with SHA256SUMS supplied with the artifacts.
tar -xzf Sunshine-PyroWave-Linux-x86_64.tar.gz
cd Sunshine-PyroWave-Linux-x86_64
./check.sh
```

The check prints missing shared libraries if any. Install those native dependencies through your package manager; your existing Sunshine installation will usually already provide them. The patched PyroWave and miniupnpc libraries are included privately. The build's glibc requirement is checked by the dynamic loader.

Back up your existing configuration **as your regular user**:

```sh
cp -a "$HOME/.config/sunshine" "$HOME/.config/sunshine.backup-$(date +%Y%m%d-%H%M%S)"
sudo ./install.sh
```

The installer puts files under `/opt/sunshine-pyrowave`, adds a separately named user service, and installs Sunshine input-device rules/module configuration. It does not overwrite `/usr/bin/sunshine`, uninstall a package, modify your streaming configuration, or switch running services. It refuses to overwrite a previous PyroWave test installation.

Switch services **without sudo**:

```sh
systemctl --user disable --now sunshine.service
systemctl --user daemon-reload
systemctl --user enable --now sunshine-pyrowave.service
systemctl --user status sunshine-pyrowave.service
journalctl --user -u sunshine-pyrowave.service -n 80 --no-pager
```

If the old service has a different name, stop/disable that service instead. If you start Sunshine through KDE Autostart or manually, stop that instance and disable its autostart entry before starting the new service. Run one Sunshine server at a time. Your normal configuration and pairing state are reused at `~/.config/sunshine`; Flatpak configurations must be copied from their application-specific directory first.

Open [the local Sunshine Web UI](https://localhost:47990), keep your existing credentials, and enable **PyroWave** under Audio/Video. Leave the PyroWave bitrate ceiling at `0` to accept the client's bitrate. A development bitrate of 200 Mbps is a starting point; choose a lower value if your connection cannot sustain it. Restart Sunshine after saving settings.

## Capture and HDR

Use a DMA-BUF capture path with `encoder = vaapi`. `capture = portal` requests an approved monitor source through the desktop portal. A correctly permissioned KMS or compatible KWin capture session is also supported. CPU/X11 capture is rejected by this native backend.

For KMS capture, Sunshine requires its existing CAP_SYS_ADMIN privilege. Apply it to the **real executable**, not the launcher, only if using KMS:

```sh
sudo setcap cap_sys_admin+p /opt/sunshine-pyrowave/libexec/sunshine
getcap /opt/sunshine-pyrowave/libexec/sunshine
systemctl --user restart sunshine-pyrowave.service
```

The real binary has a private installed-library search path, so it still resolves PyroWave when privileged execution strips LD_LIBRARY_PATH. The installation directory is root-owned. Portal capture does not require this capability; remove it with `sudo setcap -r /opt/sunshine-pyrowave/libexec/sunshine` when switching away from KMS.

HDR requires PQ/BT.2020 capture in at least RGB10; enable HDR on the desktop source display. FP16/scRGB capture and HDR-to-SDR tone mapping are not implemented. On Moonlight select PyroWave and enable HDR and YUV 4:4:4 as desired. Actual AMD/HDR desktop streaming still needs your hardware test; local GPU import, encode, decode and rendering passed.

## Roll back

```sh
systemctl --user disable --now sunshine-pyrowave.service
systemctl --user enable --now sunshine.service
```

Re-enable your former autostart method if you did not use the systemd service. Restore the backed-up config only if you want to undo your configuration edits. The original Sunshine package/binary remains installed.

To remove the test installation after rollback, remove only `/opt/sunshine-pyrowave`, `/usr/local/bin/sunshine-pyrowave`, `/usr/local/lib/systemd/user/sunshine-pyrowave.service`, `/etc/udev/rules.d/60-sunshine-pyrowave.rules`, and `/etc/modules-load.d/sunshine-pyrowave.conf`, then run `systemctl --user daemon-reload`. Do not remove the original Sunshine package's files.

## Rebuild this archive

Configure the native build with `SUNSHINE_ENABLE_PYROWAVE=ON`, `SUNSHINE_ASSETS_DIR_DEF=assets`, and `CMAKE_BUILD_WITH_INSTALL_RPATH=ON` plus `CMAKE_INSTALL_RPATH=/opt/sunshine-pyrowave/lib`. Build the `sunshine` and `web-ui` targets. Then from the workspace parent:

```sh
python3 Sunshine/packaging/linux/package-pyrowave-test.py \
  --build-dir Sunshine/cmake-build-pyrowave \
  --pyrowave-prefix build/pyrowave-install --miniupnpc-prefix build/sunshine-deps \
  --staging-dir build/Sunshine-PyroWave-package \
  --output dist/Sunshine-PyroWave-Linux-x86_64.tar.gz
```

The archive stages the complete Web UI/assets plus the two private libraries. Run `./check.sh` before switching your service.
