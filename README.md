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
bluetooth [--demo] [--ascii] [--adapter hci0] [--state-dir DIR]
```

`--demo` runs without hardware or privileges on three fake devices.
`--ascii` replaces the powerline glyphs. Pairing keys are kept per adapter
under `$XDG_STATE_HOME/bluetooth` (`~/.local/state/bluetooth`),
in a private directory locked while the program runs. Use one Bluetooth
manager per adapter: loading keys replaces the kernel's key lists.

| Key | Action |
| --- | --- |
| Up/Down, k/j | Select a device |
| s | Start or stop scanning (Classic and LE) |
| p | Pair the selected device |
| d | Disconnect the selected device |
| x | Forget the device and its pairing, after confirmation |
| o | Turn the adapter on or off |
| a | Switch to the next adapter |
| Tab | Show all devices or the paired ones |
| Esc | Cancel the pairing in progress |
| q | Quit |

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
