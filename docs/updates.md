# GitHub release updates

The updater reads the public GitHub repository `hugorosario/BrickOS`; it never queries private releases or
requires a GitHub token. No part of the updater creates commits, tags or releases.

## Using it

Install a release containing `brickos-update` through the usual SD-card package
setup. Both Stock++ and BrickOS then show **Settings → Updates**:

- **Installed build** and **Status** show the selected package and the last result.
- **Automatic checks** enables or disables background checking. It is enabled by
  default: check when the frontend starts, then every six hours. A failed check is
  retried after one minute, up to five times in a row, then every fifteen minutes. At
  boot the frontend starts the monitor before Wi-Fi has its address, so the first check
  often fails and the retry a minute later succeeds. A single monitor survives frontend/game return.
- **Check now** checks immediately, including when automatic checks are disabled.
- **What's new** shows the available update's short changelog: its tag, then the bullets
  of the changelog block in its release notes (see [Changelog](#changelog)).
  - **When.** It is dimmed until a check has found an update that Install update would take. After that update is installed, it is dimmed again.
  - **Offline.** It reads only the reply the last check saved (`BrickOS/updates/release.json`), so it works offline and downloads nothing.
  - **Notes without a block.** Such a release shows that it has no short changelog.
  - **What it shows.**
    - Only the bullet lines show, as plain text. Markdown markers, braces, backslashes, control characters and invalid UTF-8 are removed: MainUI would read `{{tags}}` and `\n` in a script's output.
    - It shows at most 40 lines and 4 KiB, and says "(changelog cut)" when there is more.
- **Install update** checks the latest stable release and asks for confirmation.
  Cancel or leaving the confirmation unanswered downloads nothing. Confirming
  downloads and prepares the package; installation happens at the next normal boot.
  The updater never reboots or stops a game automatically.

A newly discovered release raises an OSD toast pointing to Settings → Updates. It uses
the OSD's widest toast: 1000 pixels, centered at the height of MainUI's bottom notices.
There "Update available in Settings > Updates" fits on one line in the stock and BrickOS
looks; the 600-pixel toast used before 0.1.4 cut it to "Update available in Settings > Up...".
The record names every field the OSD draws from (duration, size and box), since the OSD
starts a toast at zero and would not show it. The confirmation shows the release tag. Drafts, prereleases, ambiguous package
assets and releases without a SHA-256 digest are not installable. No published
stable release is a normal status, not an installation error.

The menu also shows the installed BrickOS release and a **Component versions**
submenu. Versions come from the installed package metadata, not from its Git hash.
With the system language set to Portuguese, the screen's labels and descriptions are in
Portuguese (`lang/pt.json` in the Updates folder). Changelogs and the statuses the
updater writes stay in English.
See [component versions](component-versions.md) for bump commands and package checks.

## Runtime requirements

The device needs Wi-Fi, a correct clock, `curl` on its PATH with HTTPS support, the
firmware's CA bundle at `/etc/ssl/certs/ca-certificates.crt`, and sufficient free SD-card
space. The firmware's curl has no default CA bundle, so without `--cacert` every
verification fails (`unable to get local issuer certificate`, seen on the Brick on
2026-10-19); the updater names that bundle, or `/etc/ssl/cert.pem`, to curl. TLS verification is
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
unsupported ZIP formats and files the manifest does not list. A later release may carry
files at the card's root (listed as `../../../<path>`, as the boot script is) and more
components than this updater knows: both are accepted, the root files stay in the private
stage, and the package's own setup decides what to install. Compressed
downloads are capped at 128 MiB and expanded contents at 256 MiB. Downloads have
network timeouts and disk limits. Operations and monitors have separate locks.

Only a fully verified package gets a durable `BrickOS/updates/pending` marker, with
`pending-from` naming the build enabled when it was prepared. A package installed by hand
before the next boot wins: at boot, and at the next check, an update prepared from another
build is dropped and the status says so. A package refused by its versions does not stay
in `BrickOS/updates/packages/`, and the status says it was refused.
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
interrupted commit is retried at the next boot. When the commit itself fails after a
successful setup, the boot keeps the update's build (the boot script does not set up the
enabled one again) and the next boot retries the commit; after three failed boots the
failure is recorded, pending goes, and the enabled build is set up again. Start scripts
that the update's setup created run in that boot. The current online package and its
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

### Changelog

`CHANGELOG.md` holds a short entry for each release.
- **Format.** Under `## <version>`, newest first, there are one to six `- ` bullets of plain English, at most 160 characters each. No Markdown emphasis, links, code or HTML: the device shows them as they are.
- **Check.** `node tools/changelog.mjs check` validates the file.
- **Release builds.** A release build (`--previous`) refuses a release without a valid entry. Development builds do not read the changelog.

To publish a release's entry:
1. Write its entry when the release version is set.
2. Run `node tools/changelog.mjs notes <version>`. It prints the entry between `<!-- brickos-changelog -->` and `<!-- /brickos-changelog -->`.
3. Put that block once in the release notes as the release's short summary, for example right under "What's new". GitHub does not show the markers.
4. After publishing, read the release body through the API and check that it holds the block. Settings → Updates shows it as **What's new**.

The updater follows GitHub's selected latest stable release and compares its semantic
version with installed metadata. It rejects release/component downgrades. The rule that a
changed component needs a newer version is the packaging's (`--previous`): a card running
a development package may hold other bytes under the same versions, and still takes the
next release. The installed metadata counts only when the enabled build's own package
carries it, so the copy an older package installed by hand leaves behind is ignored.
A new metadata schema needs one manual update. Legacy installations without metadata
use asset/build checks until their first versioned update. Release ownership and TLS are the trust
boundary; this does not add a separate offline package-signing system.

API contract: [GitHub releases](https://docs.github.com/en/rest/releases/releases)
and [release asset digests](https://docs.github.com/en/rest/releases/assets).
