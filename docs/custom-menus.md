# Custom Settings menus

Stock++ and the experimental BrickOS presentation can load Settings menus from
JSON. The built-in Updates section uses this mechanism. A menu can add a tile,
open submenus or insert rows into an existing Settings section.

## Locations and reload

| Location | Definition |
| --- | --- |
| `BrickOS/settings/<name>/` on the SD card | `<name>.json`, with exactly the folder's name |
| `Apps/<app>/` on the SD card | `settings.json` |

MainUI reloads changed definitions when the Settings grid is entered again.
Scripts and relative icons resolve from the folder containing their definition.
A submenu can also name a child folder: `"menu": "more"` loads `more/more.json`
and runs that file's scripts in `more/`.

## A read-only example

Create `BrickOS/settings/Storage/Storage.json`:

```json
{
  "title": "Storage",
  "menu": [
    {
      "label": "Free SD space",
      "type": "info",
      "value": "{{free_sd}}",
      "load": "sh ./storage.sh"
    },
    {
      "label": "Storage report",
      "type": "cmd",
      "execute": "df -h /mnt/SDCARD",
      "output": "full"
    }
  ]
}
```

In the same folder, create `storage.sh` with LF line endings:

```sh
#!/bin/sh
free_sd=$(df -h /mnt/SDCARD | awk 'NR == 2 { print $4 }') || exit 1
[ -n "$free_sd" ] || exit 1
printf '{{free_sd=%s}}\n' "$free_sd"
```

Return to the Settings grid to discover Storage. Opening it loads the free-space
value; Storage report displays the real command output without changing storage.

## Rows and script values

| Type | Use | Value passed to `execute` |
| --- | --- | --- |
| `menu` | A nested `menu` array or child folder | None |
| `cmd` | Run a command with A | None |
| `toggle` | On/off, starting at `selected` | `0` or `1` |
| `select` | Choices from an `items` array, starting at `selected` | Zero-based choice index |
| `range` | A number bounded by `min`, `max` and `step` | Selected number |
| `info` | Read-only `value`, optionally filled by `load` | None |

Use `label` for the row name and `description` for its hint. An `id` lets scripts
address a row independently of its position. `disable: true` dims a row and
prevents activation. Choice/range changes are applied after a short debounce;
failed operations restore the previous displayed value.

Scripts run through `/bin/sh`, with `BRICKOS_LANGUAGE` set to the language code.
Successful loads can print tags to set values without showing those tags as text:

| Output | Meaning |
| --- | --- |
| `{{state=1}}` | Set this row's toggle, choice or number |
| `{{state(row-id)=2}}` | Set another row in the same extension |
| `{{disable(row-id)=1}}` | Disable that row |
| `{{name=value}}` | Fill `{{name}}` in the extension's text |

For commands, `output` is `none`, `line` or `full`. Errors show their output.
Commands run asynchronously; B cancels an active command. Loads have a five-second
limit, toggle/choice/range commands a thirty-second limit. Standalone commands
remain cancellable. A failed load leaves the existing displayed state unchanged.

For confirmation, a script prints `confirm:Your question` and reads stdin.
MainUI sends `A` plus a newline for confirmation, or `B` plus a newline for cancel.
The script must perform its operation only after the appropriate reply.
A question wider than the dialog is broken at spaces over up to three lines, in Stock++
and BrickOS. Only one that needs more than three lines ends its third line with "...".
Before 0.1.4 every question was cut to one line.

## Extending a built-in section

An `extend` entry adds rows before or after a built-in row, without adding a tile:

```json
{
  "extend": [
    {
      "section": "display",
      "after": "led-policy",
      "menu": [{ "label": "Policy note", "type": "info", "value": "My custom settings" }]
    }
  ]
}
```

Supported sections are `display`, `volume`, `wifi`, `hotspot`, `bluetooth`,
`themes`, `keymap`, `homepage`, `system`, `date-time`, `developer` and `device-info`.
Use `before` or `after` with the built-in row's ID; omit them to append rows.

## Icons and trust

Use `"icon": "icon.png"`, or provide `icon.png` in the menu folder. For Stock++, a
100×100 transparent image with a small white outline matches the stock Settings
icons; Updates uses a 78×78 visible glyph and a four-pixel stroke. In BrickOS the
alpha shape is tinted by the interface. Paths remain relative to the definition.

Scripts run with MainUI's device privileges. This is an extension mechanism,
not a sandbox. Use trusted scripts and keep operations explicit and cancellable.

| Updates: live information, toggle and commands | Nested component-version menu |
| --- | --- |
| ![Updates JSON menu on a real Brick](screenshots/stockpp-updates.png) | ![Component versions submenu on a real Brick](screenshots/stockpp-component-versions.png) |

These are the installed Updates extension, not the Storage example above.
Its shipped [JSON definition](../data/updater/Updates.json) and
[value-loading script](../scripts/update-menu.sh) are practical examples.
See [improvements over stock](improvements.md) and [GitHub updates](updates.md).
