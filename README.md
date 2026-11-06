# bluetooth-gpd

[![ci](https://github.com/goabonga/bluetooth-gpd/actions/workflows/ci.yml/badge.svg)](https://github.com/goabonga/bluetooth-gpd/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/goabonga/bluetooth-gpd)](https://github.com/goabonga/bluetooth-gpd/releases)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

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

The unit tests run with AddressSanitizer and UndefinedBehaviorSanitizer;
pass `SANITIZE=` to disable them on platforms that reject them.

## Code quality

```bash
make format-check   # clang-format
make lint           # clang-tidy
make format         # apply clang-format
```

## Continuous integration and releases

The `ci` workflow lints, builds (gcc and clang) and tests (with the
sanitizers) the project in an Arch Linux container, and validates the
workflows and the release configuration.

Releases are computed by [multicz](https://github.com/goabonga/multicz) from
Conventional Commits. After a green `ci` on `main`, the `release` workflow
bumps the version, tags, creates the GitHub release and attaches a
`bluetooth-gpd-<version>-linux-x86_64.tar.gz` binary built on Arch Linux, with its
SHA-256. The tarball carries a build provenance attestation, checked with
`gh attestation verify <tarball> --repo goabonga/bluetooth-gpd`. The workflows need the `GH_PAT`, `GPG_PRIVATE_KEY` and
`GPG_PASSPHRASE` repository secrets.

Commits on `main` must be signed. The branch ruleset in
`.github/rulesets/main.json` enforces this and the required checks; apply it
with `scripts/apply-rulesets.sh`. The labels used by the pipelines (`resign`,
`dependencies`, `github-actions`) are created with `scripts/apply-labels.sh`.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

[MIT](LICENSE)
