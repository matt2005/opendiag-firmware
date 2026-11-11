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

- `test_console.py`: `version` banner, `vbatt` (reports -1 mV, since battery sense is not confirmed on this build), `hsvsense` and `vbattcal` (not supported), `kline` and `j1850` usage text with no argument, `mode`, `mode slcan`, `mode elm327`, `vif`, `help`.
- `test_protocol_elm327.py`: `ATI`, `ATSP6` then `ATDPN` in one open, refused `ATSP1` and `ATSP3`, and `0100` with no bus (it must reply within 20 seconds, with no data bytes).
- `test_slcan.py`: `V`, `N`, `S6`, `O`, `C`, `F`, and listen-only `L` followed by a refused `t1230` (BELL).

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
