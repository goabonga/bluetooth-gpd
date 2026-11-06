# Security Policy

## Supported versions

Security fixes are applied only to the latest released version on the
`main` branch (and the matching release of `bluetooth-gpd`).

| Version | Supported |
| --- | --- |
| latest release | ✅ |
| older releases | ❌ |

## Reporting a vulnerability

**Please do not open a public issue.** GitHub's
[private vulnerability reporting](https://docs.github.com/en/code-security/security-advisories/guidance-on-reporting-and-writing-information-about-vulnerabilities/privately-reporting-a-security-vulnerability)
is the preferred channel:

1. Go to the repository's **Security** tab.
2. Click **Report a vulnerability**.
3. Describe the issue with reproduction steps and a suggested mitigation.

If you cannot use GitHub's form, email **goabonga@pm.me** with the same
information. PGP encryption is available on request.

You can expect an acknowledgement within **3 business days**, a triage
assessment within **10 business days**, and a fix or written mitigation
plan before any public disclosure.

## Scope

bluetooth-gpd is a small C program and library. The parts most relevant to
security are the handling of command-line arguments and the bounds of the
buffers written by `bgpd_greet`. Vulnerabilities in third-party tools
(compilers, libc) should be reported upstream.

Thanks for helping keep the project and its users safe.
