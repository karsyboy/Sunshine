#!/usr/bin/env python3
"""Create a side-by-side native Sunshine test installation for Arch/CachyOS."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tarfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--pyrowave-prefix", type=Path, required=True)
    parser.add_argument("--miniupnpc-prefix", type=Path, required=True)
    parser.add_argument("--staging-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    dynamic = subprocess.check_output(["readelf", "-d", str(args.build_dir / "sunshine")], text=True)
    if "libpyrowave-shared" not in dynamic or "/opt/sunshine-pyrowave/lib" not in dynamic:
        raise SystemExit("Build with PyroWave and the installed /opt/sunshine-pyrowave/lib RPATH")
    source = Path(__file__).resolve().parents[2]
    stage = args.staging_dir.resolve()
    if stage.exists():
        raise SystemExit("Staging directory already exists; choose a fresh directory")
    payload = stage / "sunshine-pyrowave"
    (payload / "libexec").mkdir(parents=True)
    (payload / "lib").mkdir()
    shutil.copy2(args.build_dir / "sunshine", payload / "libexec/sunshine")
    subprocess.check_call(["strip", "--strip-unneeded", str(payload / "libexec/sunshine")])
    shutil.copytree(args.build_dir / "assets", payload / "libexec/assets", symlinks=False)
    for prefix, pattern in [(args.pyrowave_prefix, "libpyrowave-shared.so*"),
                            (args.miniupnpc_prefix, "libminiupnpc.so*")]:
        for library in (prefix / "lib").glob(pattern):
            if library.is_file():
                shutil.copy2(library.resolve(), payload / "lib" / library.name)
    shutil.copy2(source / "docs/pyrowave-install.md", stage / "INSTALL.md")
    shutil.copy2(source / "src_assets/linux/misc/60-sunshine.rules", stage / "60-sunshine.rules")
    shutil.copy2(source / "src_assets/linux/misc/60-sunshine.conf", stage / "60-sunshine.conf")
    licenses = payload / "licenses"
    licenses.mkdir()
    shutil.copy2(source / "LICENSE", licenses / "Sunshine-LICENSE")
    pyro = source.parent / "pyrowave"
    shutil.copy2(pyro / "LICENSE", licenses / "PyroWave-LICENSE")
    if (pyro / "Granite/LICENSE").exists():
        shutil.copy2(pyro / "Granite/LICENSE", licenses / "Granite-LICENSE")
    (stage / "sunshine-pyrowave.service").write_text('''[Unit]
Description=Sunshine with native PyroWave HDR/444
After=graphical-session.target xdg-desktop-portal.service
PartOf=graphical-session.target

[Service]
ExecStartPre=/bin/sleep 5
ExecStart=/opt/sunshine-pyrowave/run-sunshine
Restart=on-failure
RestartSec=5

[Install]
WantedBy=graphical-session.target
''')
    scripts = {
        payload / "run-sunshine": '''#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$(readlink -f -- "$0")")" && pwd)
cd "$root/libexec"
export LD_LIBRARY_PATH="$root/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec ./sunshine "$@"
''',
        stage / "check.sh": '''#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export LD_LIBRARY_PATH="$root/sunshine-pyrowave/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
ldd "$root/sunshine-pyrowave/libexec/sunshine"
if ldd "$root/sunshine-pyrowave/libexec/sunshine" 2>&1 | grep -q 'not found'; then
    echo "Install missing native desktop dependencies before continuing." >&2
    exit 1
fi
check_dir=$(mktemp -d "${TMPDIR:-/tmp}/sunshine-pyrowave-check.XXXXXX")
trap 'rm -rf -- "$check_dir"' EXIT HUP INT TERM
XDG_CONFIG_HOME="$check_dir" XDG_DATA_HOME="$check_dir/data" "$root/sunshine-pyrowave/run-sunshine" --version
''',
        stage / "install.sh": '''#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
dest=${DESTDIR:-}
if [ -z "$dest" ] && [ "$(id -u)" -ne 0 ]; then
    echo "Run: sudo ./install.sh" >&2
    exit 1
fi
if [ -e "$dest/opt/sunshine-pyrowave" ]; then
    echo "Existing PyroWave installation found. Move it aside before installing this build." >&2
    exit 1
fi
install -d "$dest/opt" "$dest/usr/local/bin" "$dest/usr/local/lib/systemd/user"
cp -R "$root/sunshine-pyrowave" "$dest/opt/"
chmod -R go-w "$dest/opt/sunshine-pyrowave"
install -m644 "$root/sunshine-pyrowave.service" "$dest/usr/local/lib/systemd/user/"
ln -s /opt/sunshine-pyrowave/run-sunshine "$dest/usr/local/bin/sunshine-pyrowave"
install -Dm644 "$root/60-sunshine.rules" "$dest/etc/udev/rules.d/60-sunshine-pyrowave.rules"
install -Dm644 "$root/60-sunshine.conf" "$dest/etc/modules-load.d/sunshine-pyrowave.conf"
if [ -z "$dest" ]; then
    modprobe uinput
    modprobe uhid
    udevadm control --reload-rules
    udevadm trigger --subsystem-match=misc --sysname-match=uinput
    udevadm trigger --subsystem-match=misc --sysname-match=uhid
fi
echo "Installed. As your normal desktop user, follow INSTALL.md to switch services."
''',
    }
    for path, content in scripts.items():
        path.write_text(content)
        path.chmod(0o755)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(args.output, "w:gz") as archive:
        archive.add(stage, arcname="Sunshine-PyroWave-Linux-x86_64")
    print(args.output.resolve())


if __name__ == "__main__":
    main()
