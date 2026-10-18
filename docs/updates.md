# GitHub release updates

The updater reads the public GitHub repository `hugorosario/BrickOS`; it never queries private releases or
requires a GitHub token. No part of the updater creates commits, tags or releases.

## Using it

Install a release containing `brickos-update` through the usual SD-card package
setup. Both Stock++ and BrickOS then show **Settings → Updates**:

- **Installed build** and **Status** show the selected package and the last result.
- **Automatic checks** enables or disables background checking. It is enabled by
  default: check when the frontend starts, then every six hours; retry a failed
  check after fifteen minutes. A single monitor survives frontend/game return.
- **Check now** checks immediately, including when automatic checks are disabled.
- **Install update** checks the latest stable release and asks for confirmation.
  Cancel or leaving the confirmation unanswered downloads nothing. Confirming
  downloads and prepares the package; installation happens at the next normal boot.
  The updater never reboots or stops a game automatically.

A newly discovered release raises an OSD toast pointing to Settings → Updates.
The confirmation shows the release tag. Drafts, prereleases, ambiguous package
assets and releases without a SHA-256 digest are not installable. No published
stable release is a normal status, not an installation error.

The menu also shows the installed BrickOS release and a **Component versions**
submenu. Versions come from the installed package metadata, not from its Git hash.
See [component versions](component-versions.md) for bump commands and package checks.

## Runtime requirements

The device needs Wi-Fi, a correct clock, `curl` on its PATH with HTTPS support and
a working CA trust store, and sufficient free SD-card space. TLS verification is
always enabled. The updater never falls back to HTTP or an insecure TLS option.
An offline device, missing curl, invalid certificate or rate limit leaves the
installed build selected and schedules another automatic check. No token is saved.
The updater executable itself links only to libc and the firmware's zlib.

## Download and boot transaction

The updater accepts the public repository's `/releases/latest` API response and
exactly one uploaded `brickos-<12 hex build>.zip` asset. The HTTPS download URL must
belong to this repository's release assets. It checks the GitHub asset SHA-256,
size, every ZIP entry's CRC, the manifest-derived build, and each manifest size and
SHA-256. It rejects traversal, links, duplicate/case-colliding paths, unlisted files,
unsupported ZIP formats and entries outside the package's SD-card paths. Compressed
downloads are capped at 128 MiB and expanded contents at 256 MiB. Downloads have
network timeouts and disk limits. Operations and monitors have separate locks.

Only a fully verified package gets a durable `BrickOS/updates/pending` marker.
The confirmed download does not change `BrickOS/enabled`, the active bootstrap or
any running component. Cancelling the command while downloading leaves no pending
update; an incomplete private stage is reclaimed on a later confirmed attempt.

Online packages retain the complete SD-card archive layout below
`BrickOS/updates/packages/<build>/`. At the next boot, the current updater verifies
the retained ZIP and extracted files again, then runs that package's existing
`setup.sh install --boot`. The setup retains its own preflight, snapshots and
rollback behavior. A refused/failed setup keeps the old enabled marker, clears the
pending request and records a failure. A successful setup commits the bootstrap
and enabled marker atomically one file at a time, and removes pending last; an
interrupted commit is retried at the next boot. The current online package and its
predecessor are retained. The updater does not write under `/usr/trimui`.

State and preferences live in `BrickOS/updates/`; the installed programs/menu live
in the owned `BrickOS/settings/Updates/` folder. Component rollback removes the
owned menu and pending request, preserving update preferences/downloads. Full
BrickOS uninstall removes the entire BrickOS folder as before. Boot results are
in `/tmp/brickos-update-install.log` and the existing `BrickOS/setup.log`.

## Publishing

Build normally, then run `node tools/release-package.mjs --out dist` (or use
`scripts/release.ps1` / `scripts/release.sh`). Publish the resulting ZIP manually
as a non-draft, non-prerelease GitHub Release in the public repository, with its
original `brickos-<build>.zip` name. GitHub's asset API supplies the SHA-256 digest.
Use tag `v<release-version>` or `<release-version>` matching the package metadata.
Automatic checking starts seeing it after it is published. Existing installations
without the updater need one manual package update to bootstrap the feature.

The updater follows GitHub's selected latest stable release and compares its semantic
version with installed metadata. It rejects release/component downgrades and changed
component payloads without a version bump. Legacy installations without metadata
use asset/build checks until their first versioned update. Release ownership and TLS are the trust
boundary; this does not add a separate offline package-signing system.

API contract: [GitHub releases](https://docs.github.com/en/rest/releases/releases)
and [release asset digests](https://docs.github.com/en/rest/releases/assets).
