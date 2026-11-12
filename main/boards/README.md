# Board profiles

Each board has a profile header in this folder and its own board file. The profile names the
board's pins and features. The board file holds the code that differs between boards. The rules
for shared code and for adding a board are in [AGENTS.md](../../AGENTS.md).

| Board id | Profile | Notes |
| --- | --- | --- |
| `stock` | `stock.h` | The default. Upstream OpenDiag hardware, with the HS/LS drivers, boost and J1850/K-line. |
| `rejsacan` | `rejsacan_v34.h` | RejsaCAN v3.4. CAN only. |

## Choosing a board

Pick a board at configure time with `OPENDIAG_BOARD`:

```bash
idf.py -D OPENDIAG_BOARD=rejsacan build
cmake -S test/host -B test/host/build -DOPENDIAG_BOARD=rejsacan
```

Windows PowerShell uses the same commands. `-DOPENDIAG_BOARD_<ID>=ON` is the same choice.
`-DOPENDIAG_BOARD_REJSACAN=ON` (and `REJSACAN=ON` for the host suites) still works for older scripts.

Docker: `tools/docker/build.sh` builds the stock package. `tools/docker/build_rejsacan.sh` builds the
RejsaCAN package. Both run inside the pinned ESP-IDF image, and the RejsaCAN script runs the host
suites first. See [README.md](../../README.md) for the Docker and local build steps.

## RejsaCAN v3.4

- CAN only: no J1850, no K-line, no HS/LS drivers. A bus it lacks is refused through
  `vif_bus_fitted(bus)`.
- Do not flash a stock image onto a RejsaCAN. The stock image drives pins this board does not have.
- Hardware status: flashed and answering on the console, and the bench suite in `test/bench/` passed
  on the board. Vehicle use has not been tested.

## Coverage on stock and RejsaCAN

- Both builds meet the gate in [AGENTS.md](../../AGENTS.md): at least 95% total line coverage of
  `main/*.c`, and at least 90% per file.
- Stock total is 97.4%. Five refusal lines cannot be reached on stock, because the check they sit
  behind (`!vif_bus_fitted(bus)`) is always true on stock (`BOARD_HAS_KLINE` and `BOARD_HAS_J1850`
  are 1). They are:
  - `elm327_set_protocol`'s "Protocol not supported on this board" branch in `main/elm327_at.c`
    (the log and `return -1`).
  - `vif_bus_open`'s `ESP_ERR_NOT_SUPPORTED` return in `main/vif.c`.
  - The `BUS_ERR_UNSUPPORTED` returns in `vif_bus_stats` and `vif_bus_reset_stats` in `main/vif.c`.
  RejsaCAN covers them. Do not add a GCOVR marker for them, and do not add a non-stock test build to
  force them on stock. Revisit this if the bus-fitted check becomes a runtime value.
