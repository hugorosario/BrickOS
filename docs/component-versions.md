# Component versions

`versions.json` at the repository root is the source of truth for the release and
each component's [Semantic Version](https://semver.org/spec/v2.0.0.html). The first
versioned development snapshot starts at `0.1.0`; this is not a claim of device
acceptance or a published release. Version zero denotes an evolving interface.

| Component ID | Versioned payload |
| --- | --- |
| `mainui` | MainUI, Stock++ and BrickOS presentations, frontend install/start/wrapper files |
| `inputd` | Input daemon, its installer and startup templates |
| `osdd` | OSD daemon, its installer and startup templates |
| `ledd` | LED daemon, its installer and startup templates |
| `tmenu` | Standalone in-game menu library and installer |
| `gameswitcher` | GameSwitcher app, session library, supervisor, scripts and app resources |
| `uninstall` | Uninstaller program, app resources and rollback scripts |
| `updater` | Updater program, Settings menu and boot activation script |
| `setup` | Package installer and top-level boot entrypoint |
| `assets` | Bootlogos, splashscreens, loading presets, Settings icons and wallpapers |

Stock++ and BrickOS share a binary, so they share the MainUI version. GameSwitcher's
three native artifacts share one version because they are deployed as one component.
The release version describes the whole distribution; individual components can
remain unchanged across releases. Hash-based build IDs and per-file SHA-256 values
remain separate and identify the exact bytes.

## Changing versions

```sh
node tools/component-versions.mjs list
node tools/component-versions.mjs bump osdd patch
node tools/component-versions.mjs bump release minor
node tools/component-versions.mjs set mainui 0.2.0-rc.1
node tools/component-versions.mjs check
```

Use patch for compatible fixes, minor for compatible functionality and major for
incompatible contracts. These are explicit source-file edits: commands never stage,
commit, tag, push or publish. They reject malformed versions and backwards changes.
SemVer precedence includes prerelease ordering and ignores build metadata. Storage
is bounded to 63 ASCII bytes per version and nine digits per core number.

`bump`/`set` regenerate `src/common/versions.generated.h`. After editing the JSON by
hand, run `node tools/component-versions.mjs generate`. The normal build wrappers
do this too; standalone container builds refuse a stale header. Node.js is needed
on the host for version generation and packaging, not on the device.

Every native production artifact embeds its component ID/version in a private
`.brickos.version` ELF section. Each executable accepts `--version` before opening
SDL, input devices or the SD card. MainUI's existing `--versions` continues reporting
SDL library versions. Library exports are unchanged. Inspect a library on the host:

```sh
node tools/component-versions.mjs inspect bin/tmenu/libtmenu.so
```

Only the component version is embedded, so increasing the overall release version
does not change an otherwise identical component binary.

## Package and publication checks

Build normally, then package with:

```sh
node tools/release-package.mjs --out dist
node tools/release-package.mjs --out dist --previous dist/brickos-<previous-build>.zip
```

The second command validates against a previous versioned ZIP; `--previous` also
accepts an extracted package `versions.json`. It requires a newer release version,
refuses component downgrades, and requires a newer component version whenever that
component's payload fingerprint changes. Development packaging without a baseline
is allowed, but stale/mislabeled native binaries are always rejected.

For a full build and comparison in one command, use
`scripts/release.ps1 -Previous dist/brickos-<previous-build>.zip` on Windows or
`sh scripts/release.sh --previous dist/brickos-<previous-build>.zip` on a shell.

Changes to shared code or duplicated helper scripts can affect multiple components.
The baseline comparison names each component needing a bump; rebuild after changing
its version. The shared GameSwitcher/menu code, for example, can affect both
`tmenu` and `gameswitcher`.

The package includes `BrickOS/package/<build>/versions.json`, covered by the ordinary
manifest. It records release/component versions and deterministic per-component
fingerprints over the sorted file paths, hashes, sizes and modes. Every payload file
belongs to exactly one component; the metadata file itself is excluded from its own
fingerprints. Same inputs give the same metadata and package identity.

Publish a public GitHub release with tag `v<release-version>` (or `<release-version>`)
and the generated `brickos-<build>.zip`. The tag and package version must agree.
The automatic updater accepts only stable releases. A prerelease package needs a
GitHub prerelease and manual installation with the current stable update channel.

## Installed versions and updates

The installer copies the package metadata to `BrickOS/versions.json` in its existing
transaction. Snapshots restore it after a failed install, and component rollback
removes it. Older packages without metadata remain installable manually.

**Settings → Updates** shows the BrickOS release. **Component versions** shows all
ten component versions in both Stock++ and BrickOS. The updater's `versions` command
prints the complete installed metadata; `status` supplies the menu placeholders.

The updater compares semantic release versions before downloading. After checking
the complete ZIP and ordinary file manifest it also requires the package version
to match the GitHub tag and refuses release and component downgrades; components it
does not know are accepted. The rule that a changed payload needs a newer version stays
with `--previous` at packaging, since a development card may hold other bytes under the
same versions. The check repeats before boot installation.
An interrupted final commit can retry the same verified versions. Installations
without version metadata bootstrap through their first versioned package and use
the earlier asset/build checks until then.
