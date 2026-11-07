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

`make install` installs `bluetooth`, `bluetooth-daemon` and the systemd
user unit of the daemon under `$(PREFIX)` (default `/usr/local`), honouring
`DESTDIR`.

## Usage

Two programs share the work. Neither uses BlueZ, libbluetooth or D-Bus: the
daemon talks to the Linux kernel Bluetooth Management API directly.

- `bluetooth-daemon` owns the adapter, the pairing keys and the connections
  of the paired BLE devices. It keeps them connected, tries again every five
  seconds when a link drops, and keeps running when no interface is open.
- `bluetooth` is the terminal interface. It needs no privilege: it shows the
  state the daemon sends and turns keys into commands for it.

They talk through the Unix socket `$XDG_RUNTIME_DIR/bluetooth.sock`, which
only the owner can use.

The kernel requires `CAP_NET_ADMIN` to open the management socket. The
daemon drops every capability once it is open, so grant it to the binary:

```bash
sudo make install-capabilities
systemctl --user enable --now bluetooth-daemon
bluetooth
```

```bash
bluetooth-daemon [--adapter hci0] [--state-dir DIR] [--socket PATH] [--trace FILE]
bluetooth [--demo] [--ascii] [--socket PATH]
```

`bluetooth --demo` runs without the daemon on three fake devices. `--ascii`
replaces the powerline glyphs. The daemon keeps the pairing keys per adapter
under `$XDG_STATE_HOME/bluetooth` (`~/.local/state/bluetooth`), in a
private directory locked while it runs. Use one Bluetooth manager per
adapter: loading keys replaces the kernel's key lists. `bluetoothd` receives
the same authentication requests and refuses them, so pairing fails at once:
the interface shows a warning when it runs, and
`sudo systemctl mask --now bluetooth.service` fixes it (stopping it is not
enough, D-Bus starts it again). `--trace FILE` logs the management packets
(without keys or PINs). Without the daemon, the interface waits for it and
connects as soon as it runs.

| Key | Action |
| --- | --- |
| Up/Down, k/j | Select a device |
| s | Start or stop scanning (Classic and LE) |
| p | Pair the selected device |
| c | Connect a paired BLE device and serve it as an input device (HID over GATT) |
| d | Disconnect the selected device, which then stays disconnected |
| x | Forget the device and its pairing, after confirmation |
| o | Turn the adapter on or off |
| a | Switch to the next adapter |
| Tab | Show all devices or the paired ones |
| Esc | Cancel the pairing in progress |
| q | Quit the interface; the daemon keeps the devices connected |

A BLE keyboard, mouse or game controller (HID over GATT, such as a Flipper
Zero in keyboard mode) is connected by the daemon right after its pairing
and again whenever it is paired and in range: the daemon reads its report
map and forwards its reports to the kernel through `/dev/uhid`. `make
install` installs the udev rule `70-bluetooth-uhid.rules`, which gives that
device to the user of the active local session (`sudo udevadm control
--reload && sudo udevadm trigger /dev/uhid`, or a new login, applies it).
Classic devices are not connected yet.

Pairing prompts (confirmation code, PIN of 1 to 16 characters, passkey of up
to 6 digits) open a dialog in the interface; unsolicited requests are
rejected. Address types are `BR` (Classic), `LE-P` (public LE) and `LE-R` (random LE); the flags are
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
