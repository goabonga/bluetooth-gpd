# bluetooth-gpd

[![ci](https://github.com/goabonga/bluetooth-gpd/actions/workflows/ci.yml/badge.svg)](https://github.com/goabonga/bluetooth-gpd/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/goabonga/bluetooth-gpd)](https://github.com/goabonga/bluetooth-gpd/releases)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

Terminal Bluetooth manager for Linux, built on ctui.

## Build

Requires a C11 compiler, GNU make, pkg-config, ncursesw, libsbc (`sbc`), alsa-lib and the
[ctui](https://github.com/goabonga/ctui) library, found through `ctui.pc`
when installed, else in `../ctui`. `CTUI_DIR=path` always uses that checkout
instead, even when ctui is installed, to develop against a local ctui; it is
built on demand.

```bash
make
./build/bluetooth --demo
./build/bluetooth --version
```

`make install` installs `bluetooth`, `bluetooth-daemon`, the ALSA plugin, the
systemd service of the daemon and its support files (a sysusers.d file for its
user, a udev rule and a modules-load.d file for `/dev/uhid`) under `$(PREFIX)`
(default `/usr/local`), honouring `DESTDIR`. `make uninstall` removes them.

## Usage

Two programs share the work. Neither uses BlueZ, libbluetooth or D-Bus: the
daemon talks to the Linux kernel Bluetooth Management API directly.

- `bluetooth-daemon` owns the adapter, the pairing keys and the connections
  of the paired BLE devices. It keeps them connected, tries again every five
  seconds when a link drops, and keeps running when no interface is open.
- `bluetooth` is the terminal interface. It needs no privilege: it shows the
  state the daemon sends and turns keys into commands for it.

The daemon is a system service. It runs as its own user, `bluetooth`, which
systemd gives the `CAP_NET_ADMIN` capability the kernel requires to open the
management socket; the daemon drops it as soon as the socket is open. Its
pairing keys are in `/var/lib/bluetooth`, private to it.

The interface and the other programs talk to it through Unix sockets in
`/run/bluetooth` (`bluetooth.sock` for the control protocol), which the
users of the group `bluetooth` can use: add yours to it.

```bash
sudo make install PREFIX=/usr
sudo systemd-sysusers
sudo usermod -aG bluetooth $USER      # then log in again
sudo systemctl mask --now bluetooth.service
sudo modprobe uhid
sudo udevadm control --reload && sudo udevadm trigger /dev/uhid
sudo systemctl enable --now bluetooth-daemon
bluetooth
```

```bash
bluetooth-daemon [--adapter hci0] [--state-dir DIR] [--socket PATH] [--trace FILE]
bluetooth [--demo] [--ascii] [--socket PATH]
```

`bluetooth --demo` runs without the daemon on three fake devices. `--ascii`
replaces the powerline glyphs. The daemon keeps the pairing keys per adapter
in the directory of `--state-dir` (`$XDG_STATE_HOME/bluetooth` when it is
not given), private and locked while it runs; the service uses
`/var/lib/bluetooth`. `BLUETOOTH_RUNTIME_DIR` replaces `/run/bluetooth`
for the programs that look for the sockets. Use one Bluetooth manager per
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
| v | Make the adapter visible for two minutes, or hide it, so that other devices can pair |
| p | Pair the selected device |
| c | Connect a paired device: a BLE device is served as an input device (HID over GATT), a Classic one opens its audio stream (A2DP) |
| t | Play a three second test tone on the selected Classic audio device |
| + / - | Raise or lower by 5 % the volume of the selected audio device, else of the first one |
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
map and forwards its reports to the kernel through `/dev/uhid`. The udev rule
`70-bluetooth-uhid.rules` gives that device to the group `bluetooth`, and
the module `uhid` is loaded at boot (`modprobe uhid` loads it at once).

A Classic headset or speaker (A2DP sink) is connected with `c`: the daemon
negotiates an SBC stream (44.1 or 48 kHz, joint stereo first, bitpool up to
53) and `t` plays a test tone through it, encoded with libsbc and paced in
real time. Up to four audio devices can be connected at the same time, each
with its own stream, volume and microphone. The volume is set on the
device when it has an absolute volume (AVRCP), and the buttons of the device
then update the volume shown; otherwise the daemon applies it before the
encoding, as a gain from -50 dB at 1 % to silence at 0.

## Playing audio

Applications play through the ALSA PCM `bluetooth_out` (`make install`
puts `libasound_module_pcm_bluetooth.so` in `$(ALSA_PLUGIN_DIR)`, default
`$(PREFIX)/lib/alsa-lib`, and the definitions in
`$(PREFIX)/share/alsa/alsa.conf.d/99-bluetooth.conf`; ALSA only loads
plugins from its own directory, so use `PREFIX=/usr`):

```bash
aplay -D bluetooth_out music.wav
```

```
application -> bluetooth_out -> plug -> bluetooth plugin --(socket)--> bluetooth-daemon -> SBC -> headset
```

`bluetooth_out` plays on the device connected last. To choose one when
several are connected, define a PCM with the address of the device:

```
pcm.speaker { type plug slave.pcm { type bluetooth device "AA:BB:CC:DD:EE:FF" } }
```

The plugin talks to the audio socket of the daemon,
`/run/bluetooth/bluetooth-audio.sock` (`--audio-socket PATH`; `server "PATH"`
in the PCM definition). It sends `BGPDPCM2` and the address of the device,
14 hexadecimal digits as in the control protocol (all zeros for the device
connected last) and a newline, receives the sample rate negotiated with the
device (4 bytes, little-endian, 0 when it cannot be served) and then writes
signed 16-bit little-endian stereo PCM, which the daemon encodes and sends at
the speed of the stream. The PCM cannot be opened until the device is
connected (`c` in the interface), and it has only that rate: `plug` converts
the rest. The stream is suspended once the client has left and its audio has
been played. A program such as `mixer-daemon` can use `bluetooth_out` as its
output PCM.

Pairing prompts (confirmation code, PIN of 1 to 16 characters, passkey of up
to 6 digits) open a dialog in the interface. A device that starts the pairing
itself (after `v` made the adapter visible) is asked the same way, as an
incoming pairing; requests are rejected when no interface is open to answer
them. Address types are `BR` (Classic), `LE-P` (public LE) and `LE-R` (random LE); the flags are
P paired, C connected and S key saved.

## Headset microphone

Recording from the PCM `bluetooth_in` starts everything the microphone needs
on the audio device connected last, or the one named by `device` in the
PCM definition (`c` first):

```bash
arecord -D bluetooth_in -f S16_LE -r 16000 -c 1 -d 5 voice.wav
```

The daemon finds the Hands-Free service of the headset (SDP), connects its
RFCOMM channel, answers its AT commands to establish the service level
connection, agrees on the codec (mSBC wide band at 16 kHz when the headset
supports it, else CVSD at 8 kHz) and opens the SCO voice link. The PCM then
receives the microphone as mono 16-bit samples at the rate of the link
(`plug` converts to the rate you ask), and opening it can take several
seconds. Closing it ends the voice and hands-free links. The audio socket
for it is `/run/bluetooth/bluetooth-capture.sock` (`--capture-socket PATH`;
`capture_server "PATH"` in the PCM definition): the client sends `BGPDCAP2`,
the address of the device and a newline as for playback, receives the sample
rate (4 bytes, little-endian; 0 when there is no headset or the link failed)
and then the PCM. Calls and the earpiece are
not handled: the sound still plays through the A2DP stream, and some
headsets lower its quality or pause it while the voice link is open.

## Control protocol

Other programs can follow and drive the daemon on its control socket
(`/run/bluetooth/bluetooth.sock`, text lines). The daemon sends a snapshot
of its state whenever it changes: tab-separated records `state`, `adapter`,
`status`, `warning`, `prompt`, one `device` per device
(`ADDR14 flags rssi name`, the address being 14 hexadecimal digits: six
little-endian bytes and the type), one `audio` record per
audio device in use (`ADDR14 connecting|ready|streaming volume name`, the
volume being 0 to 100, or -1 when unknown) and `end`. A snapshot replaces the previous one. The client sends one
command per line: `scan`, `power`, `adapter`, `visible`, `cancel`,
`accept [input]`, `reject`, `pair`, `connect`, `disconnect`, `unpair` and `tone`
followed by an address, and `volume ADDR14 N` (N from 0 to 100). `include/ipc.h` is the reference.

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
