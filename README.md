# bluetooth-gpd

Terminal Bluetooth manager for Linux, built on ctui.

## Build

```bash
make
./build/bluetooth --version
```

`make install` installs the binary under `$(PREFIX)/bin` (default
`/usr/local`), honouring `DESTDIR`.

## Tests

```bash
make test
```

The unit tests run with AddressSanitizer and UndefinedBehaviorSanitizer.
