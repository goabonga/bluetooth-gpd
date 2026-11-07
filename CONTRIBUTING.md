# Contributing to bluetooth-gpd

Thanks for taking the time to contribute. This document is the short version
of how to propose a change and what the project expects in return.

## Code of Conduct

Participation in this project is governed by the
[Code of Conduct](CODE_OF_CONDUCT.md). By contributing you agree to abide by
its terms.

## Development setup

You need a C11 compiler (gcc or clang), `make`, `pkg-config`, ncursesw,
[libsbc](https://github.com/bluez/sbc) (`sbc`), alsa-lib,
[ctui](https://github.com/goabonga/ctui) (installed, or checked out next to
this repository in `../ctui`; `make CTUI_DIR=path` always uses that checkout),
`clang-format` and `clang-tidy`.

```bash
git clone https://github.com/goabonga/bluetooth-gpd.git
cd bluetooth-gpd
make
```

The `ci` and `release` workflows install the version of ctui published on the
AUR, building its package with `makepkg`. Set the repository variable
`CTUI_TAG` (for example `v0.1.0`) to pin another tag: it is installed from the
upstream sources, unless the AUR publishes exactly that version.

## Quality gates

Before pushing, make sure your code passes the same gates the `ci` workflow
runs:

```bash
make format-check
make lint
make test
uv tool run multicz validate --strict
```

## Commit messages

Commit messages MUST follow
[Conventional Commits](https://www.conventionalcommits.org/). They drive the
version bump and CHANGELOG computed by
[multicz](https://github.com/goabonga/multicz).

| Type | Effect on version | Use it for |
| --- | --- | --- |
| `feat` | minor | new capability |
| `fix` | patch | bug fix |
| `perf` | patch | performance improvement |
| `refactor`, `docs`, `test`, `chore`, `ci`, `build`, `style` | none | maintenance |
| `feat!` / `BREAKING CHANGE:` | major | incompatible change |

Only commits that touch the tracked paths (`src/**`, `include/**`,
`Makefile`) trigger a release. Do not append `Co-Authored-By` trailers.
Commits must be signed: add the `resign` label to a pull request to have
its commits re-signed.

## Releasing

Releases are automated. A push to `main` starts a strictly ordered
pipeline: `resign-main` (re-signs unsigned commits), then `ci`, then
`release`, each starting when the previous one completed. The `release`
workflow runs `multicz bump` (signed commit and tag), creates the
GitHub release and attaches the binary built on Arch Linux. Maintainers do
not bump versions or edit the changelog by hand.

## Reporting bugs and asking for features

Please open a GitHub issue. For security-sensitive reports, follow
[SECURITY.md](SECURITY.md) instead of the public tracker.
