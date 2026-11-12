# Local build, test and export

`tools/local/build_all.sh` builds, tests and exports every release artifact from one commit:
the stock and RejsaCAN firmware ZIPs, the J2534 driver ZIP, the test and coverage reports, and
SHA256 hashes. Use it before flashing a board or handing out a build.

It runs in a clean clone of `HEAD`, so the artifacts match a commit exactly.

## Requirements

| Tool | Used for | Linux | macOS | Windows |
|---|---|---|---|---|
| bash, git | script, clone | system | system | WSL (Debian) |
| cmake, ninja, gcc, make | host suites, driver tests | `apt install cmake ninja-build gcc make` | `brew install cmake ninja gcc make` | in WSL, same as Linux |
| gcov | coverage (`gcov-14` is used if present) | with gcc | `brew install gcc` (use `GCOV=gcov-14`) | in WSL |
| python3 with pip | gcovr 8.8, installed into a temporary folder | system | system | in WSL |
| docker | firmware and driver builds | Docker Engine | Docker Desktop | Docker Engine **inside WSL** |

On Windows, run the script inside WSL, not with Windows Docker Desktop. The WSL Docker engine is what
the builds were verified with.

Windows PowerShell:

```powershell
wsl.exe -e bash -lc 'cd /mnt/c/Users/<you>/install/repos/opendiag/opendiag-firmware && tools/local/build_all.sh'
```

Linux and macOS:

```bash
tools/local/build_all.sh
```

## What it does

Each step prints `PASS` or `FAIL` in a summary at the end. The script keeps going after a failed
step, so one run shows every failure. It exits 0 only if every step passed.

1. **Clean clone.** Clones the repository, checks out `HEAD`, and runs `git submodule update --init --recursive`.
   Uncommitted changes are not included; the script prints a warning when there are any.
2. **Coverage tool.** Installs gcovr 8.8 with `pip install --target` into the temporary work folder.
3. **Host suite, stock.** Builds `test/host` with `-DCOVERAGE=ON`, runs `ctest`, then writes the coverage report.
4. **Host suite, RejsaCAN.** The same with `-DOPENDIAG_BOARD=rejsacan`.
5. **Driver Linux tests.** `make -C j2534_driver test`.
6. **Firmware, stock (Docker).** `tools/docker/build.sh` in the pinned ESP-IDF 6.1 image. Writes `opendiag-firmware.zip`.
7. **Firmware, RejsaCAN (Docker).** `tools/docker/build_rejsacan.sh` in the same image. Writes `opendiag-firmware-rejsacan.zip`.
8. **Driver, Windows (Docker).** Builds `j2534_driver/Dockerfile` and runs `j2534_driver/tools/docker_build.sh`.
   Writes `opendiag-j2534-driver.zip`.
9. **Hashes.** Writes `SHA256SUMS` over every file in the output folder, and `BUILD_INFO.txt` with the commit and branch.

### Coverage gates

Coverage is measured for `main/*.c` with gcc `--coverage` and gcovr:

- RejsaCAN: total line coverage at least **95%**, and every file at least **90%**.
- Stock: every file at least **90%**. Stock has five refusal lines that cannot be reached on the stock
  build; see [AGENTS.md](../AGENTS.md).

A failed gate is a `FAIL` line in the summary and a `GATE FAIL` line in the log.

## Output

The output folder is `build/artifacts/<short commit>/` in the repository (inside `build/`, so Git
ignores it). Use `--out DIR` to choose another folder.

```
build/artifacts/<commit>/
  BUILD_INFO.txt                 commit, branch, build time, whether uncommitted changes were excluded
  SHA256SUMS                     SHA-256 of every other file in this folder
  firmware/opendiag-firmware.zip             stock firmware
  firmware/opendiag-firmware-rejsacan.zip    RejsaCAN v3.4 firmware
  driver/opendiag-j2534-driver.zip           J2534 driver for Windows
  reports/ctest-stock.txt, ctest-stock.xml   stock host suite (log and JUnit)
  reports/ctest-rejsacan.txt, ctest-rejsacan.xml
  reports/coverage-stock.txt, coverage-stock.xml       per-file coverage (text and Cobertura)
  reports/coverage-rejsacan.txt, coverage-rejsacan.xml
  reports/driver-linux-test.txt  driver Linux test log
  reports/build-*.log, driver-image.log  build logs
```

Check a ZIP against the hashes before flashing:

- Linux and macOS: `sha256sum -c SHA256SUMS`
- Windows PowerShell: `Get-FileHash <file> -Algorithm SHA256` and compare with `SHA256SUMS`

## Options

- `--out DIR`: write the artifacts to `DIR`.
- `--keep-work`: keep the temporary clone and build trees for inspection. The script prints the path.

Environment variables:

- `CC`: C compiler for the host suites (default `gcc`; use `gcc-14` on macOS if needed).
- `GCOV`: gcov executable for coverage (default `gcov-14`, then `gcov`).

## Notes

- A run takes a long time: the two firmware builds compile ESP-IDF, and each uses several GB of memory.
  Close other memory-heavy programs first. If the system stops the run under memory pressure, the
  summary is incomplete; rerun the script.
- The Docker steps write as root inside the containers. The script removes those files with a
  container, so it can clean up on Windows and Linux without `sudo`.
- The firmware scripts refuse to run when the project folder has a `build/` folder. The script
  removes it before each firmware build, and each build runs in a fresh clone of `HEAD`.
- The script does not commit, tag or push anything.
