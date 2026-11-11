# Flashing the RejsaCAN v3.4 firmware

This covers the `opendiag-firmware-rejsacan.zip` made by `tools/docker/build_rejsacan.sh`, on Windows, Linux and macOS. Flash only that ZIP. A stock OpenDiag image drives pins this board does not have. See the RejsaCAN warning in the README.

## Status

- **Flash:** done on a virgin board on Windows (COM8, esptool 5.6.0). All three images wrote and the hash check passed.
- **Boot:** confirmed on Windows. After pressing RESET once, the board exposed two serial ports (COM9 and COM10) with the OpenDiag USB ID `1209:D1A6`.
- **Console:** confirmed. The console is COM9 (described as `DEBUG`), and `version` returns a reply on it. The reply text has not been copied into this guide yet.

## 1. Check the ZIP

Unzip `opendiag-firmware-rejsacan.zip` into an empty folder, then check the images against `SHA256SUMS`.

- Linux and macOS: `sha256sum -c SHA256SUMS`
- Windows PowerShell: for each line, run `Get-FileHash <file> -Algorithm SHA256` and compare with the first column. The file names are in the second column.

## 2. The board and its buttons

- The USB-C connector is the ESP32-S3's native USB. In download mode it appears as a **USB JTAG/serial debug unit** (USB vendor `303A`, product `1001`).
- **PROG** is GPIO 0, the BOOT strap. **RESET** restarts the chip.
- To enter download mode by hand: hold **PROG**, press and release **RESET**, then release **PROG**.

## 3. Unplug the vehicle, then plug in USB

Flashing is bench work. Keep the adapter off the car, use a data cable (not charge-only), and close any other program that has the board's serial port open.

## 4. Install esptool

esptool 5 or later matches the ESP-IDF 6.1 build. Use a virtual environment so the install stays self-contained.

- **Windows** (PowerShell):

  ```powershell
  python -m venv esptool-venv
  .\esptool-venv\Scripts\python.exe -m pip install esptool
  ```

- **Linux and macOS:**

  ```bash
  python3 -m venv esptool-venv
  ./esptool-venv/bin/python -m pip install esptool
  ```

## 5. Find the port

- **Windows:** Device Manager → Ports (COM & LPT), or run `esptool-venv\Scripts\python.exe -m serial.tools.list_ports -v`. Look for `USB JTAG/serial debug unit` or `USB Serial Device`. The board used **COM8** here. Ignore Bluetooth ports.
- **Linux:** `ls /dev/ttyACM*`. Add your user to the `dialout` group if access is denied, then log out and back in.
- **macOS:** `ls /dev/cu.usbmodem*`.

## 6. Check that the chip answers

Read-only check, nothing is written:

```bash
esptool --chip esp32s3 --port PORT chip-id
```

Replace `PORT` with `COM8`, `/dev/ttyACM0` or `/dev/cu.usbmodemXXXX`. On Windows, run it as `esptool-venv\Scripts\esptool.exe`. The chip is an ESP32-S3 and reports its MAC address. If it does not answer, enter download mode by hand (section 2) and try again.

## 7. Flash a virgin board

A blank board has no application, so every image must be written. Run this from the unzipped folder:

```bash
esptool --chip esp32s3 --port PORT write-flash \
    0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 opendiag.bin
```

On Windows use `esptool-venv\Scripts\esptool.exe` and `COM8` (or your port) in place of `esptool` and `PORT`. The write runs in a few seconds and ends with `Hash of data verified.`

## 8. Start the firmware

Press **RESET** once, or unplug and reconnect USB. Then:

- The board should show two new serial ports: the protocol port (described as `COM`) and the console (described as `DEBUG`). On Windows these are two new COM ports. On Linux and macOS, two new `ttyACM` or `cu.usbmodem` devices.
- Open the **console** and type `version`. It reports the firmware version, which should match the `version` field in `manifest.json` (`v1.0.3-22-g64d34fa` for the build from commit `64d34fa`). The console is the port whose description is `DEBUG`. Identify it by that description, not by its number: on Windows it was COM9 here, with the protocol port on COM10, and the upstream manual's example shows the same layout.

If no second port appears, the firmware has not started. Reflash with the same command. Check the ZIP first, then run `chip-id` to confirm the board still answers.

## 9. Update a board that already runs firmware

Writing the three images does not erase the stored settings, so pairings and calibration are kept as long as the partition table does not change.

Put the board in download mode first:

- **From the console:** open the console port, type `reboot dl`, and wait for it to reappear as a USB JTAG/serial device.
- **By hand:** if the console does not respond, use the PROG and RESET sequence in section 2.

Then run step 7 again, and step 8 to start the new firmware.

## Troubleshooting

- **esptool cannot connect.** Use the PROG and RESET sequence. Try another cable or USB port, and check that nothing else holds the serial port.
- **Flash fails partway.** Repeat step 7. Keep the board on a steady supply, because a power loss during a write can leave it unresponsive until you reflash.
- **Board boots into download mode every time.** GPIO 0 is being read as low. Check that PROG is not stuck pressed, and that the board is not held in download mode by the host.
- **No second port after flashing.** The firmware has not started. Reflash, then press RESET.
