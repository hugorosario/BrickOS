# Release package

Run `scripts/release.ps1` on Windows or `sh scripts/release.sh` on a shell with
Docker available. Node.js is required for packaging. The command builds all ten
production artifacts, checks their ARM64 format and writes
`dist/brickos-<build>.zip`.

If the current sources have already been built, package the existing binaries
with:

```sh
node tools/release-package.mjs --out dist
```

The packager validates inputs and the generated ZIP before writing it. The build
identifier is derived from the package manifest, which records file hashes and
modes.

## Contents

The ZIP contains these paths relative to the SD card root:

| Path | Purpose |
| --- | --- |
| `BrickOS/package/<build>/` | Binaries, installer, rollback helpers and visual resources |
| `BrickOS/enabled` | Marker identifying the selected package |
| `System/starts/brickos-00-setup.sh` | Startup installer and rollback entrypoint |

Visual resources include bootlogos, splashscreens, loading-screen presets,
settings icons and wallpapers. Stock firmware binaries, themes, fonts and sounds
are supplied by the device's existing firmware.

The startup script uses the enabled marker to select and install or update the
package during boot. Setup results are recorded in `BrickOS/setup.log`. The
package also supplies the Uninstall BrickOS app and rollback scripts.

The package records a release version and independent component versions. See
[component versions](component-versions.md) for explicit version changes and
comparison against a previous release. [GitHub updates](updates.md) describes
automatic checks, installation confirmation and boot-time activation.
