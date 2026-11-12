# AGENTS.md

Guidance for coding agents working on the OpenDiag firmware (ESP32-S3, ESP-IDF 6.1, GPL-3.0).
The logger tools and the RejsaCAN port notes are in the separate Zafirab_canbus repository.

## Boards

Exactly one board is built per configuration. Everything that differs between boards lives in
board files and profiles. Shared code never names a board.

| File | Role |
| --- | --- |
| `main/boards/<name>.h` | Profile: `BOARD_ID`, `BOARD_NAME`, the `BOARD_HAS_*` features, `BOARD_CAN_BUSES`, and every `PIN_*` (`PIN_NONE` where the board lacks the pin). |
| `main/boards/board_spec.h` | The spec: every name a profile must define, with what it means. |
| `main/boards/board_check.h` | Stops the build if a profile misses a name. Derives `BOARD_CAN_ONLY`. |
| `main/boards/board_<id>.c` | The board's own code: the hooks in `board_hooks.h`, and the `board.h` functions that differ (sense inputs, HS/LS drivers, calibration). |
| `main/board.c` | Shared: bring-up order, NVS calibration, calibration commands. |
| `main/boards/board_select.cmake` | Finds the profiles, picks one, writes the generated `board_profile.h`. No board names in its logic. |
| `test/host/boards/<id>.cmake` | The host suite list for the board. |

Choose a board with `-DOPENDIAG_BOARD=<id>` (default `stock`). `-DOPENDIAG_BOARD_<ID>=ON` is the same
choice; `-DOPENDIAG_BOARD_REJSACAN=ON` is kept for existing scripts.

```bash
idf.py -D OPENDIAG_BOARD=rejsacan build
cmake -S test/host -B test/host/build -DOPENDIAG_BOARD=rejsacan
```

These work unchanged in PowerShell, bash and zsh.

### Rules for shared code

- Read the `BOARD_HAS_*`, `BOARD_CAN_BUSES`, `BOARD_CAN_ONLY` flags and the `PIN_*` names. Do not
  test `OPENDIAG_BOARD_*` or any board id outside the profiles, board files and `board_select.cmake`.
- Refuse a missing feature with a check on its flag, `if (!BOARD_HAS_X)`, not with `#ifdef`. The
  stock build folds the branch away, and the RejsaCAN build keeps the refusal.
- A bus the board lacks is refused through `vif_bus_fitted(bus)`. Protocol code asks that, and never
  the board.
- Driver code for a bus a board lacks may still compile, using its pins. Those pins are never driven
  there, because the bus is refused before it opens.

### Adding a board

1. `main/boards/<name>.h`: define `BOARD_ID "<id>"` (lowercase), `BOARD_NAME`, every name listed in
   `board_spec.h`. Use `PIN_NONE` for absent pins. A missing name fails at compile time.
2. `main/boards/board_<id>.c`: implement `board_pins_init`, `board_adc_init` and
   `board_adc_calibrated` (`board_hooks.h`), and the `board.h` functions. Start from
   `board_rejsacan.c` for a board without the HS/LS drivers, `board_stock.c` for one with them.
3. `test/host/boards/<id>.cmake`: set `SUITES`. Add suites for the board's refusals, modelled on the
   RejsaCAN suites (`test_vif_rejsacan.c`, `test_j2534_rejsacan.c`).
4. No CMake edit is needed: boards are found from the files. Configure and check for the line
   `OpenDiag board: <id>`.
5. Update `README.md` and this file in the same change. The board is not verified on hardware until
   flashed and tested.

## Stock behaviour

- The stock build must not change. Any change to stock-visible behaviour needs a test that shows it.
- A refactor keeps the stock host count (34 of 34 at the baseline) and the stock firmware compiling.
- Do not weaken a test to make a change pass. If a test must change, say why in the commit or report.
- Known stock gap, accepted: five refusal lines are unreachable on stock, so stock line coverage is
  about 97.4%, not 100%. They are `elm327_set_protocol`'s "Protocol not supported on this board"
  branch (`main/elm327_at.c`, the log and `return -1`), `vif_bus_open`'s `ESP_ERR_NOT_SUPPORTED`
  return and the `BUS_ERR_UNSUPPORTED` returns in `vif_bus_stats` and `vif_bus_reset_stats`
  (`main/vif.c`). They sit behind `!vif_bus_fitted(bus)`, which on stock is always true because
  `BOARD_HAS_KLINE` and `BOARD_HAS_J1850` are constant 1 there, so the branch is dead on stock.
  RejsaCAN covers them. Do not add a GCOVR marker for them (RejsaCAN reaches them), and do not add a
  non-stock test build to force them on stock. Revisit this if the bus-fitted check becomes a runtime
  value.

## Tests and coverage

- Host suites: `ctest --test-dir test/host/build`. They run offline, with fakes for the hardware.
  They do not compile `main/boards/*.c` or the ESP-IDF include paths, so a firmware build is the
  check for those (see Docker builds).
- RejsaCAN coverage gate: at least 95% total line coverage of `main/*.c`, and at least 90% per file.
  A change must not lower either figure. A change that adds code adds tests in the same change.
- Measure with GCC `--coverage`, then gcovr (`python -m pip install gcovr`):

```bash
cmake -S test/host -B build/cov -DOPENDIAG_BOARD=rejsacan -DCOVERAGE=ON -DCMAKE_C_COMPILER=gcc
cmake --build build/cov
ctest --test-dir build/cov
python -m gcovr --root . --filter "main/.*\.c$" --print-summary build/cov
```

  On Windows run these in WSL (or any shell with gcc). On macOS use `brew install gcc` and
  `-DCMAKE_C_COMPILER=gcc-14`, or the matching gcc version.
- GCOVR markers: `/* GCOVR_EXCL_START -- reason */` and `/* GCOVR_EXCL_STOP */`, only around code the
  measured build cannot run. The reason names that build. Never put one on a line a test on that build
  can reach: cover it with a test instead. A marker also drops its lines from every other build's
  count, so keep markers tight.

## Hardware

- Host tests check the logic against fakes. They do not prove the board. Say in each report what is
  verified on a real board and what is host-only.
- The bench tests in `test/bench/` need a flashed board on a serial port. They are not run by the host
  suites, and no bench result is implied by a passing host run.

## Code and docs

- Docstrings describe contracts and non-obvious behaviour. Comments explain protocol bytes, AT
  sequences and timing where the bytes are not obvious. Match the surrounding style.
- The ELM327 init commands are a sequence from the ELM327 datasheet. Do not change them without
  understanding monitor mode.
- Update the docs in the same change when a change alters a board, an option, a build flag, a
  warning, an output or a test: `README.md`, the user docs under `docs/`, and this file.
- Commands must work on Windows, Linux and macOS, or show each OS. Use placeholders for serial ports
  and say how to find them on each OS.
- Format with `tools/format.sh` (clang-format, `.clang-format`). It is a bash script: run it on Linux,
  macOS or WSL.

## Docker builds

- `tools/docker/build.sh` builds stock, and `tools/docker/build_rejsacan.sh` builds RejsaCAN. The
  RejsaCAN script runs the host suites first. Both copy the whole tree, so finish edits first.
- The scripts need a full Git clone with a `.git` directory. A git worktree has a `.git` file, and
  the RejsaCAN script refuses it. Build from a full clone, or copy the tree to one.
- On Windows, run Docker builds inside WSL Debian (`wsl -d Debian -- bash -c '...'`) with WSL's own
  Docker engine. Do not use Docker Desktop for these builds.
