#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# Builds, tests and exports everything from a clean clone of one commit:
#   firmware/  stock and RejsaCAN firmware ZIPs (Docker, pinned ESP-IDF image)
#   driver/    J2534 driver ZIP for Windows (Docker, j2534_driver/Dockerfile)
#   reports/   host test results (ctest log and JUnit XML), coverage reports,
#              and the driver's Linux test log
#   SHA256SUMS hashes of every file in the output folder
#   BUILD_INFO.txt  the commit the artifacts were built from
#
# Usage: tools/local/build_all.sh [--out DIR] [--keep-work]
#   --out DIR     output folder (default: build/artifacts/<short commit>)
#   --keep-work   keep the temporary clone and build trees for inspection
#
# The artifacts come from a clean clone of HEAD. Uncommitted changes are not
# included; the script warns when there are any.
#
# Needs: bash, git, cmake, ninja, gcc, gcov (gcov-14 if present), make,
# python3 with venv, and docker. On Windows run it inside WSL:
#   wsl.exe -e bash -lc 'cd /mnt/c/Users/<you>/install/repos/opendiag/opendiag-firmware && tools/local/build_all.sh'
#
# Exit status is 0 only if every step passes, including the coverage gates
# (total line coverage of main/*.c at least 95% for RejsaCAN; every file at
# least 90% for both builds).
set -uo pipefail

usage() { sed -n '3,20p' "$0" | sed 's/^# \{0,1\}//'; }

out=""
keep_work=0
while (( $# )); do
    case "$1" in
        --out) out=${2:?--out needs a folder}; shift 2 ;;
        --keep-work) keep_work=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done

repo=$(git rev-parse --show-toplevel 2>/dev/null) || {
    echo "Run this inside the opendiag-firmware git repository." >&2
    exit 1
}
cd "$repo" || exit 1
commit=$(git rev-parse HEAD)
short=$(git rev-parse --short=12 HEAD)
branch=$(git branch --show-current)
dirty=$(git status --porcelain)
if [[ -n $dirty ]]; then
    echo "Warning: uncommitted changes are NOT in the artifacts (built from $short):" >&2
    sed 's/^/  /' <<< "$dirty" >&2
fi

for tool in git cmake ninja gcc make python3 docker; do
    command -v "$tool" >/dev/null || { echo "Missing tool: $tool" >&2; exit 1; }
done
gcov_exe=${GCOV:-$(command -v gcov-14 || command -v gcov)}
cc=${CC:-gcc}
debian_image=debian:bookworm-slim@sha256:5ae3c39ebd15e229dcedd5cee596b2497182493d41ff162e824ba13fc1b2b867
if command -v sha256sum >/dev/null; then sha_cmd=(sha256sum); else sha_cmd=(shasum -a 256); fi

out=${out:-$repo/build/artifacts/$short}
mkdir -p "$out" || exit 1
out=$(cd "$out" && pwd) || exit 1

work=$(mktemp -d "${TMPDIR:-/tmp}/opendiag-build.XXXXXX") || exit 1
cleanup() {
    if (( keep_work )); then
        echo "Kept work folder: $work"
    else
        docker_rm "$work" "/w/* /w/.[!.]*" 2>/dev/null
        rm -rf "$work"
    fi
}
trap cleanup EXIT

src=$work/src
status=0
summary=()

step() {  # step NAME COMMAND [ARGS...]: run one step, record PASS or FAIL
    local name=$1; shift
    echo; echo "== $name"
    if "$@"; then
        summary+=("PASS  $name")
    else
        summary+=("FAIL  $name")
        status=1
    fi
}

prepare_source() {
    git clone -q --no-checkout "$repo" "$src" &&
        git -C "$src" checkout -q "$commit" &&
        git -C "$src" submodule update -q --init --recursive
}

setup_gcovr() {  # pinned gcovr 8.8, the version the logger's AGENTS.md records
    python3 -m pip install -q --target "$work/pylib" gcovr==8.8
}

gcovr_run() {  # run the pinned gcovr from the temporary library folder
    PYTHONPATH="$work/pylib" python3 -m gcovr "$@"
}

docker_rm() {  # docker_rm HOST_DIR "PATHS": delete paths inside HOST_DIR as root,
    # because the container writes files that the host user cannot remove
    docker run --rm -v "$1:/w" "$debian_image" sh -c "rm -rf $2"
}

# Host suite for one board, built with coverage, then ctest reports and the
# coverage report. Arguments after NAME go to cmake (board selection).
host_suite() {
    local name=$1; shift
    local bld=$work/cov-$name
    cmake -S "$src/test/host" -B "$bld" -G Ninja -DCOVERAGE=ON -DCMAKE_C_COMPILER="$cc" "$@" \
        > "$out/reports/build-$name.log" 2>&1 &&
        cmake --build "$bld" >> "$out/reports/build-$name.log" 2>&1 || return 1
    ctest --test-dir "$bld" --output-on-failure --timeout 60 \
        --output-junit "$out/reports/ctest-$name.xml" > "$out/reports/ctest-$name.txt" 2>&1
    local rc=$?
    tail -n 3 "$out/reports/ctest-$name.txt"
    (( rc == 0 )) || return 1
    coverage_report "$name" "$bld"
}

coverage_report() {  # coverage_report NAME BUILD_DIR: text, XML, and the gate check
    local name=$1 bld=$2
    local json=$work/coverage-$name.json
    gcovr_run --root "$src" --filter '.*main/.*\.c$' \
        --gcov-executable "$gcov_exe" \
        --txt "$out/reports/coverage-$name.txt" \
        --cobertura "$out/reports/coverage-$name.xml" \
        --json-summary "$json" --print-summary "$bld" || return 1
    python3 - "$json" "$name" <<'PY'
import json, sys

path, name = sys.argv[1], sys.argv[2]
with open(path) as f:
    data = json.load(f)
total = data.get("line_percent", 0.0)
files = sorted(data.get("files", []), key=lambda f: f.get("line_percent", 0.0))
print(f"{name}: total lines {total:.2f}%")
for f in files[:3]:
    print(f"  lowest: {f['filename']} {f.get('line_percent', 0.0):.2f}%")
fail = []
if not files or data.get("line_total", 0) == 0:
    fail.append("no coverage data for main/*.c (check the filter)")
if name == "rejsacan" and total < 95.0:
    fail.append(f"total {total:.2f}% is below 95%")
for f in files:
    if f.get("line_percent", 0.0) < 90.0:
        fail.append(f"{f['filename']} {f.get('line_percent', 0.0):.2f}% is below 90%")
for line in fail:
    print("  GATE FAIL:", line)
sys.exit(1 if fail else 0)
PY
}

docker_firmware() {  # docker_firmware SCRIPT: build one firmware ZIP in a fresh tree
    # build.sh refuses to run when the project folder has a build/ folder, so
    # remove it first (the host suites above did not create one).
    docker_rm "$src" /w/build || return 1
    docker run --rm --platform linux/amd64 \
        -e IDF_TARGET=esp32s3 -e CCACHE_DISABLE=1 -e TZ=UTC \
        -e GIT_CONFIG_COUNT=1 -e GIT_CONFIG_KEY_0=safe.directory -e GIT_CONFIG_VALUE_0='*' \
        -v "$src:/project" -v "$out/firmware:/output" -w /project \
        espressif/idf@sha256:f81b6e077e6af93c312181cd7d4e7936cfbafc5c7f77cbacba8fdd34994f218a \
        /opt/esp/entrypoint.sh bash "tools/docker/$1"
}

driver_windows() {  # J2534 driver ZIP built in the pinned Debian image
    docker build --platform linux/amd64 -f "$src/j2534_driver/Dockerfile" \
        -t opendiag-j2534-driver "$src" > "$out/reports/driver-image.log" 2>&1 &&
        docker run --rm --init --platform linux/amd64 \
            -e LANG=C.UTF-8 -e LC_ALL=C.UTF-8 -e TZ=UTC \
            -v "$out/driver:/output" --entrypoint bash \
            opendiag-j2534-driver j2534_driver/tools/docker_build.sh
}

driver_linux() {  # driver host tests built with coverage, then the driver coverage report
    # COVERAGE=1 adds --coverage in the driver Makefile. Flags in the environment
    # would reach the test/host CMake build and break its contract library.
    local bld=$work/drv-cov
    make -C "$src/j2534_driver" test COVERAGE=1 BUILD_DIR="$bld" > "$out/reports/driver-linux-test.txt" 2>&1 || return 1
    grep -c '^PASS' "$out/reports/driver-linux-test.txt" | sed 's/^/PASS lines: /'
    gcovr_run --root "$src/j2534_driver" --filter '.*(driver|common|legacy|platform|config)/.*\.(c|cpp)$' \
        --gcov-executable "$gcov_exe" \
        --txt "$out/reports/coverage-driver.txt" \
        --cobertura "$out/reports/coverage-driver.xml" \
        --json-summary "$work/coverage-driver.json" --print-summary "$bld" || return 1
    python3 - "$work/coverage-driver.json" <<'PY'
import json, sys

data = json.load(open(sys.argv[1]))
files = sorted(data.get("files", []), key=lambda f: f.get("line_percent", 0.0))
print(f"driver: total lines {data.get('line_percent', 0.0):.2f}% (no gate; firmware gates apply to main/)")
for f in files[:3]:
    print(f"  lowest: {f['filename']} {f.get('line_percent', 0.0):.2f}%")
if not files or data.get("line_total", 0) == 0:
    print("  GATE FAIL: no driver coverage data (check the filter)")
    sys.exit(1)
PY
}

write_manifest() {
    {
        echo "commit: $commit"
        echo "branch: ${branch:-detached}"
        echo "built: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
        if [[ -n $dirty ]]; then echo "uncommitted changes: excluded"; else echo "uncommitted changes: none"; fi
    } > "$out/BUILD_INFO.txt"
    (cd "$out" && find . -type f ! -name SHA256SUMS | sed 's|^\./||' | sort \
        | xargs "${sha_cmd[@]}" > SHA256SUMS)
}

docker_rm "$out" "/w/firmware /w/driver /w/reports /w/SHA256SUMS /w/BUILD_INFO.txt" || exit 1
mkdir -p "$out/firmware" "$out/driver" "$out/reports"

echo "Building $short${branch:+ on $branch} into $out"
step "clean clone of $short" prepare_source || { echo "Cannot prepare the source clone." >&2; exit 1; }
step "tools for coverage (gcovr 8.8)" setup_gcovr || { echo "Cannot install gcovr 8.8 (needs python3 venv)." >&2; exit 1; }
step "host suite, stock (coverage)" host_suite stock
step "host suite, RejsaCAN (coverage)" host_suite rejsacan -DOPENDIAG_BOARD=rejsacan
step "driver Linux tests and coverage" driver_linux
step "firmware stock (Docker)" docker_firmware build.sh
step "firmware RejsaCAN (Docker)" docker_firmware build_rejsacan.sh
step "driver Windows build (Docker)" driver_windows

write_manifest

echo
echo "== Summary"
printf '%s\n' "${summary[@]}"
echo "Artifacts: $out"
exit $status
