# bluetooth-gpd

[![ci](https://github.com/goabonga/bluetooth-gpd/actions/workflows/ci.yml/badge.svg)](https://github.com/goabonga/bluetooth-gpd/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/goabonga/bluetooth-gpd)](https://github.com/goabonga/bluetooth-gpd/releases)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

Terminal Bluetooth manager for Linux, built on ctui.

## Build

Requires a C11 compiler, GNU make, pkg-config, ncursesw and the
[ctui](https://github.com/goabonga/ctui) library, found through `ctui.pc`
when installed, else in `../ctui`. `CTUI_DIR=path` always uses that checkout
instead, even when ctui is installed, to develop against a local ctui; it is
built on demand.

```bash
make
./build/bluetooth --demo
./build/bluetooth --version
```

`make install` installs the binary under `$(PREFIX)/bin` (default
`/usr/local`), honouring `DESTDIR`.

## Usage

The program talks to the Linux kernel Bluetooth Management API directly: no
BlueZ service, libbluetooth or D-Bus. The kernel requires `CAP_NET_ADMIN`
to open the management socket; the program drops every capability once it
is open.

```bash
bluetooth [--demo] [--ascii] [--adapter hci0] [--state-dir DIR] [--trace FILE]
```

`--demo` runs without hardware or privileges on three fake devices.
`--ascii` replaces the powerline glyphs. Pairing keys are kept per adapter
under `$XDG_STATE_HOME/bluetooth` (`~/.local/state/bluetooth`),
in a private directory locked while the program runs. Use one Bluetooth
manager per adapter: loading keys replaces the kernel's key lists. `bluetoothd` receives the same
authentication requests and refuses them, so pairing fails at once: the
program warns when it runs, and `systemctl stop bluetooth` fixes it.
`--trace FILE` logs the management packets (without keys or PINs).

| Key | Action |
| --- | --- |
| Up/Down, k/j | Select a device |
| s | Start or stop scanning (Classic and LE) |
| p | Pair the selected device |
| c | Connect a paired BLE device and serve it as an input device (HID over GATT) |
| d | Disconnect the selected device |
| x | Forget the device and its pairing, after confirmation |
| o | Turn the adapter on or off |
| a | Switch to the next adapter |
| Tab | Show all devices or the paired ones |
| Esc | Cancel the pairing in progress |
| q | Quit |

A BLE keyboard, mouse or game controller (HID over GATT, such as a Flipper
Zero in keyboard mode) is connected right after its pairing and with `c`:
the program reads its report map and forwards its reports to the kernel
through `/dev/uhid`. `make install` installs the udev rule
`70-bluetooth-uhid.rules`, which gives that device to the user of the active
local session (`sudo udevadm control --reload && sudo udevadm trigger
/dev/uhid`, or a new login, applies it). A device
stays connected only while the program runs, and Classic devices are not
connected yet.

Pairing prompts (confirmation code, PIN of 1 to 16 characters, passkey of up
to 6 digits) open a dialog; unsolicited requests are rejected. Address types
are `BR` (Classic), `LE-P` (public LE) and `LE-R` (random LE); the flags are
P paired, C connected and S key saved.

## Tests

```bash
make test
```

The unit tests run with AddressSanitizer and UndefinedBehaviorSanitizer;
pass `SANITIZE=` to disable them on platforms that reject them.

## Code quality

```bash
make format-check   # clang-format
make lint           # clang-tidy
make format         # apply clang-format
```

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

[MIT](LICENSE)
