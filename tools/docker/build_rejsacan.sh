#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Builds the RejsaCAN v3.4 firmware package (OPENDIAG_BOARD_REJSACAN=ON) and
# writes it to the output directory as opendiag-firmware-rejsacan.zip. The
# stock package is built by tools/docker/build.sh.
set -euo pipefail

usage() {
    echo "Usage: docker compose run --rm --build firmware-rejsacan [--verify]"
    echo "Create the output directory first: mkdir -p build/docker"
    echo "--verify builds twice from scratch and compares the ZIPs."
}

verify=false
case "${1:-}" in
    --verify) verify=true ;;
    -h|--help) usage; exit 0 ;;
    "") ;;
    *) usage >&2; exit 2 ;;
esac
if (( $# > 1 )); then
    usage >&2
    exit 2
fi

if [[ $(uname -m) != x86_64 ]]; then
    echo "The release builder requires platform: linux/amd64." >&2
    exit 1
fi

if [[ ! -d .git ]] || [[ $(git rev-parse --is-shallow-repository) != false ]]; then
    echo "Use a full Git clone with tags and recursive submodules." >&2
    exit 1
fi
submodules=$(git submodule status --recursive)
if grep -q '^[-+U]' <<< "$submodules"; then
    echo "Initialize the pinned submodules: git submodule update --init --recursive" >&2
    exit 1
fi

output_dir=${OPENDIAG_OUTPUT_DIR:-/output}
if [[ ! -d "$output_dir" || ! -w "$output_dir" ]]; then
    echo "Create the output directory first: mkdir -p build/docker" >&2
    exit 1
fi
if [[ -e build ]]; then
    echo "A clean container is required: docker compose run --rm --build firmware-rejsacan" >&2
    exit 1
fi

export SOURCE_DATE_EPOCH
SOURCE_DATE_EPOCH=$(git log -1 --format=%ct)

# The host suites run against the RejsaCAN defines before anything is packaged.
cmake -S test/host -B build/host -G Ninja -DREJSACAN=ON
cmake --build build/host --parallel 2
ctest --test-dir build/host --output-on-failure --timeout 30

cp dependencies.lock build/dependencies.lock
build_package() {
    local build_dir=$1

    # Keep developer menuconfig settings out of release builds.
    idf.py -B "$build_dir" -D "SDKCONFIG=$PWD/$build_dir/sdkconfig" \
        -D "SDKCONFIG_DEFAULTS=$PWD/sdkconfig.defaults" \
        -D OPENDIAG_BOARD_REJSACAN=ON reconfigure
    if ! cmp -s dependencies.lock build/dependencies.lock; then
        echo "ESP-IDF changed dependencies.lock; update and commit it before building." >&2
        exit 1
    fi
    grep -qx 'CONFIG_APP_REPRODUCIBLE_BUILD=y' "$build_dir/sdkconfig"
    idf.py -B "$build_dir" firmware-package
    cmp dependencies.lock build/dependencies.lock

    # Rename the manifest so the web installer shows which board this is, then
    # rebuild SHA256SUMS and the ZIP under the board's own name.
    python - "$build_dir/firmware" "$build_dir/opendiag-firmware-rejsacan.zip" <<'PY'
import hashlib, json, sys, zipfile
from pathlib import Path

directory, archive = Path(sys.argv[1]), Path(sys.argv[2])
manifest = json.loads((directory / "manifest.json").read_text())
manifest["name"] = "OpenDiag Firmware (RejsaCAN v3.4)"
(directory / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
files = {p.name: p.read_bytes() for p in directory.iterdir() if p.name != "SHA256SUMS"}
sums = "".join(f"{hashlib.sha256(d).hexdigest()}  {n}\n" for n, d in sorted(files.items()))
(directory / "SHA256SUMS").write_text(sums)
files["SHA256SUMS"] = sums.encode()
with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as bundle:
    for n, d in sorted(files.items()):
        info = zipfile.ZipInfo(n)
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = 0o100644 << 16
        bundle.writestr(info, d)
PY
    (cd "$build_dir/firmware" && sha256sum --check SHA256SUMS)
    python -m zipfile --test "$build_dir/opendiag-firmware-rejsacan.zip"
}

build_package build/first
if "$verify"; then
    build_package build/second
    cmp build/first/opendiag-firmware-rejsacan.zip build/second/opendiag-firmware-rejsacan.zip
    echo "Reproducibility verified: both clean builds produced identical ZIPs."
fi

staged_archive=$(mktemp "$output_dir/.opendiag-firmware-rejsacan.XXXXXX")
trap 'rm -f -- "$staged_archive"' EXIT
cp build/first/opendiag-firmware-rejsacan.zip "$staged_archive"
chmod 644 "$staged_archive"
chown --reference="$output_dir" "$staged_archive"
mv -f -- "$staged_archive" "$output_dir/opendiag-firmware-rejsacan.zip"
echo "Firmware package: build/docker/opendiag-firmware-rejsacan.zip"
