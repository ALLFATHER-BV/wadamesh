# Contributing to wadamesh

Thanks for your interest — wadamesh is open and collaborative by design, and
contributions are very welcome.

## License of contributions

wadamesh is licensed under **GPL-3.0-or-later** (see [LICENSE](LICENSE)). By
submitting a contribution you agree that it is licensed under the same terms
(inbound = outbound). This keeps every build and fork open.

Sign off your commits to certify you wrote (or have the right to submit) the
change, per the [Developer Certificate of Origin](https://developercertificate.org/):

```
git commit -s -m "..."     # adds a Signed-off-by line
```

New source files should carry an SPDX header:

```c
// SPDX-License-Identifier: GPL-3.0-or-later
```

Files derived from MeshCore keep their original **MIT** header — don't relicense
upstream code; only your own additions are GPL.

## How we work

A few principles (borrowed from how this project is built):

- **One topic per pull request.** Small, focused changes that do one thing are
  easy to review and easy to revert. Open a separate PR per concern.
- **Discuss next steps, keep the actual change scoped.** It's great to note what
  could come next — just don't bundle it into the same diff.
- **Prefer pulling in a library over hand-rolling.** If a maintained library
  solves it, depend on it (and add it to [NOTICE](NOTICE)) rather than reinventing.
- **Don't regress what ships.** wadamesh runs on real devices; changes should keep
  both boards (LilyGo T-Deck and Heltec V4 TFT) building and behaving.

## Building

The firmware is a PlatformIO project that depends on a MeshCore fork via
`lib_deps`. Build/flash instructions land in the README as the split from
meshcomod completes. In the meantime, open an issue if you'd like to help.

## Reporting issues

Bug reports and feature ideas are welcome in the issue tracker — please include
your board, firmware version, and steps to reproduce.
