# RejsaCAN v3.4 bench tests

Pytest checks for a RejsaCAN v3.4 running OpenDiag, connected over USB on the bench. They test the console, the ELM327 protocol port and the SLCAN front end. There is no vehicle and no CAN traffic: nothing is expected on the bus, and no CAN frame is asserted.

They are separate from the host unit tests in `test/host/`, which do not run them.

## Requirements

- Python 3.9 or newer.
- `pip install pytest pyserial` (on Windows, `py -m pip install pytest pyserial` if `python` is not on PATH).
- The board on USB. It presents two serial ports: the console (described as **DEBUG**) and the protocol port (described as **COM**).

## Find the two ports

Run this on any OS. It lists every serial port with its description:

```
python -m serial.tools.list_ports -v
```

Then pick the platform-specific view if you want more detail:

- Windows (PowerShell): `Get-CimInstance Win32_PnPEntity | Where-Object { $_.Name -match 'COM\d+' } | Select-Object Name, PNPDeviceID`. Device Manager, under "Ports (COM & LPT)", shows the same two entries.
- Linux: `ls -l /dev/serial/by-id/`, or `dmesg | tail` right after plugging the board in. Ports appear as `/dev/ttyACM0` and `/dev/ttyACM1` (the numbers can vary). Your user needs access: `sudo usermod -aG dialout $USER`, then log out and back in.
- macOS: `ls /dev/cu.usb*`. Use the `cu.` names, not `tty.`.

The DEBUG port is the console (`OPENDIAG_CONSOLE_PORT`). The COM port is the protocol port (`OPENDIAG_DATA_PORT`). If you are unsure which is which, the console answers `version` with the OpenDiag banner, and the protocol port answers `ATI` with `ELM327 v2.3`.

## Set the ports and run

Windows, PowerShell:

```
cd test\bench
$env:OPENDIAG_CONSOLE_PORT="COM9"
$env:OPENDIAG_DATA_PORT="COM10"
python -m pytest -v
```

Windows, cmd.exe:

```
cd test\bench
set OPENDIAG_CONSOLE_PORT=COM9
set OPENDIAG_DATA_PORT=COM10
python -m pytest -v
```

Linux, bash:

```
cd test/bench
export OPENDIAG_CONSOLE_PORT=/dev/ttyACM1
export OPENDIAG_DATA_PORT=/dev/ttyACM0
python3 -m pytest -v
```

macOS, zsh or bash:

```
cd test/bench
export OPENDIAG_CONSOLE_PORT=/dev/cu.usbmodemXXXX
export OPENDIAG_DATA_PORT=/dev/cu.usbmodemYYYY
python3 -m pytest -v
```

Use the port names from your own machine (the ones above are examples). A run takes about 25 seconds.

If a variable is unset or empty, every test that needs that port is skipped with a message saying which variable is missing. The console tests need only `OPENDIAG_CONSOLE_PORT`. The ELM327 tests need only `OPENDIAG_DATA_PORT`. The SLCAN tests need both.

Clear the variables to check the skip path:

- PowerShell: `Remove-Item Env:OPENDIAG_CONSOLE_PORT, Env:OPENDIAG_DATA_PORT -ErrorAction SilentlyContinue`
- cmd.exe: `set OPENDIAG_CONSOLE_PORT=` and `set OPENDIAG_DATA_PORT=`
- bash, zsh: `unset OPENDIAG_CONSOLE_PORT OPENDIAG_DATA_PORT`

## What is checked

- `test_console.py`: `version` banner, `kline` and `j1850` usage text with no argument, `mode`, `mode slcan`, `mode elm327`, `vif`, `help`. On the RejsaCAN build only: `vbatt` (reports -1 mV, since battery sense is not confirmed), `hsvsense` and `vbattcal` (ESP_ERR_NOT_SUPPORTED).
- `test_protocol_elm327.py`: `ATI`, `ATSP6` then `ATDPN`, and `0100` with no bus (it must reply within 20 seconds, with no data bytes). On the RejsaCAN build only: refused `ATSP1` and `ATSP3`.
- `test_slcan.py`: `V`, `N`, `S6`, `O`, `C`, `F`, and listen-only `L` followed by a refused `t1230` (BELL).

The build is read from the `version` banner first. A check that differs between the RejsaCAN and stock builds skips on the other build with a reason. The stock build's replies have not been captured on hardware, so those checks skip on stock with "no captured stock reply". A skip is not a pass.

## Commissioning report

`commission.py` runs the same suite and writes a Markdown report for the board. It reads the identity first (read-only), then runs the tests with pytest and parses the result. It never sends `can` or a pin command.

It needs the same ports and the same Python as the suite, with `pytest` and `pyserial` installed. Use the Python of a venv that has the two packages:

- Windows, PowerShell: `py -m venv $env:USERPROFILE\.venvs\rejsacan-bench`, then `& $env:USERPROFILE\.venvs\rejsacan-bench\Scripts\python.exe -m pip install pytest pyserial`. The Python is `%USERPROFILE%\.venvs\rejsacan-bench\Scripts\python.exe`.
- Linux, bash: `python3 -m venv ~/.venvs/rejsacan-bench`, then `~/.venvs/rejsacan-bench/bin/python -m pip install pytest pyserial`. The Python is `~/.venvs/rejsacan-bench/bin/python`.
- macOS, zsh or bash: as Linux. The Python is `~/.venvs/rejsacan-bench/bin/python`.

Find the two ports as in [Find the two ports](#find-the-two-ports). Then run from `test/bench`. Replace the port names, and the text in angle brackets, with your own.

Windows, PowerShell:

```
cd test\bench
& $env:USERPROFILE\.venvs\rejsacan-bench\Scripts\python.exe commission.py --console-port COM9 --data-port COM10 --operator "<name>" --unit-label "<bench number>"
```

Linux, bash:

```
cd test/bench
~/.venvs/rejsacan-bench/bin/python commission.py --console-port /dev/ttyACM1 --data-port /dev/ttyACM0 --operator "<name>" --unit-label "<bench number>"
```

macOS, zsh or bash:

```
cd test/bench
~/.venvs/rejsacan-bench/bin/python commission.py --console-port /dev/cu.usbmodemXXXX --data-port /dev/cu.usbmodemYYYY --operator "<name>" --unit-label "<bench number>"
```

`--operator` and `--unit-label` are optional. If `--console-port` and `--data-port` are omitted, the run reads `OPENDIAG_CONSOLE_PORT` and `OPENDIAG_DATA_PORT`, as the suite does. `--out-dir` sets the folder (default `test/bench/reports`). The run takes about 30 seconds.

Each run writes two files into the out folder, named `commissioning-<UTC time>-<board MAC>` (the SLCAN serial if the MAC is not read):

- `.md`: the report. It has the run date and time (UTC), the operator and unit label, the ports used, the pytest command, the firmware version (git describe), the build detected (RejsaCAN or stock), the board type, the board MAC, ESP-IDF, uptime, reset reason, free heap, the link owner and front end, the SLCAN serial, the results table (test, result, duration, notes), totals, an overall PASS or FAIL line, a "Not verified" section, and a sign-off block with blank Operator signature and Date fields.
- `.junit.xml`: the raw JUnit XML from pytest.

The board MAC is the per-chip identifier. The firmware writes its eFuse base MAC into the USB serial string, so the report reads it from the console port's USB descriptor without opening the port. The SLCAN serial is the `N` reply, a fixed firmware string that does not identify one board. The report says so.

A skipped check is not a pass. Any skip makes the overall result FAIL, and the report gives the number of skipped checks.

Exit codes:

- `0`: PASS. Every check ran and passed, with no skips.
- `1`: not a PASS. A check failed or errored, a check was skipped, or the run did not finish (the report says why).
- `2`: no ports configured. The message says what to set, and nothing is written.

The `reports/` folder is git-ignored (`test/bench/.gitignore`), because the reports carry the board's MAC address. Do not commit them.

## Safety

- The tests never energise a connector pin. They do not send `hsset`, `lsset`, `pinoff`, or any K-line or J1850 payload. `kline` and `j1850` are only called with no argument, to print usage.
- The only transmit-like command is `t1230`, sent in listen-only mode, where it must be refused with BELL.
- `O` opens the CAN channel in normal mode, which can acknowledge frames on the bus. Keep the board off any vehicle while these tests run, and the test closes the channel with `C` straight away.
- Each test restores `mode elm327` in teardown, even when it fails. If a run is interrupted and the board stays in SLCAN, run `python -m pytest -k mode_slcan` again, or send `mode elm327` to the console port by hand.
- Each open of the protocol port restarts the firmware link. The fixtures open each port once per test and close it in teardown. Do not run two copies of the tests at once, and close any other program that holds the same port (a serial monitor, PuTTY, esptool, the web installer). If a port is refused with access denied, something else has it open.
- Ports open with DTR and RTS set to True before the open, so opening does not reset the board.
- Nothing personal is printed. Port names come only from the environment variables.

## Not covered

- Real bus traffic, the RS pin level, battery sense accuracy, and flashing. These need the vehicle or a separate bench setup, as listed in [../FIRMWARE.md](../FIRMWARE.md).
- Timing of the ELM327 protocol. The tests check replies, not timing, except for the 20-second limit on `0100`.
