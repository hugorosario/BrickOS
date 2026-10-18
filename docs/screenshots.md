# Device screenshots

All published images originate from captures of a real TrimUI Brick. They retain
the native 1024 × 768 resolution. The selection favours Stock++ and its stock-style
companion surfaces: 20 of the 24 images. The remaining four show the experimental
BrickOS desktop and Appearance, GameSwitcher and the removal app.

The captures span different development builds from 1–18 October 2026; they do
not identify the exact binaries in a ZIP generated later from this checkout.
Installed themes and settings belong to the capture sessions. Earlier captures
may show fewer options than the current implementation.

Ten new Stock++ captures were taken on 18 October, using release **0.1.2**,
package **eeb0a0b3f2be**, MainUI **0.1.1** and updater **0.1.1**. They show custom
menus, Bluetooth's available state, input mapping, LED options, visual previews,
frontend selection and installed versions. Wi-Fi was disabled; no update was
downloaded and no accessory was paired, connected or removed for these captures.
Bluetooth was already enabled and the saved headset was disconnected.

The Custom LED policy was selected temporarily with the LEDs off, then the
original Default policy was restored byte-for-byte. Visual selections were only
previewed; the applied bootlogo, splashscreen and loading preset were retained.
The frontend selector focuses BrickOS while Stock++ remains active. Local user
presets visible in a browser are not a list of everything included in the ZIP.

## Obscuring game content

Twenty-two images are unedited copies. Two have opaque masks applied by script:

- **GameSwitcher:** the complete preview and game title are covered with solid
  colour. Every pixel outside those two rectangles remains unchanged.
- **In-game menu:** the game frame is covered across the whole capture, including
  where it showed through the translucent panel. Captured menu labels, selection
  outline and panel outline are retained over an opaque background.

The masks have no transparency or blur and no substituted game imagery. No
generated or recreated screenshot is used. Captures containing game-list titles
and cover art were removed from the public screenshot selection. Original
captures and source/published hashes are kept in ignored local maintainer files.
The ten new Settings captures contain no game titles or game imagery and need
no masks. Existing masked captures retain their previous published hashes.

| File | Capture date | Context | Publication edit |
| --- | --- | --- | --- |
| [stockpp-games.png](screenshots/stockpp-games.png) | 2026-10-12 | Stock++ Games screen | Unedited capture |
| [stockpp-keymap.png](screenshots/stockpp-keymap.png) | 2026-10-09 | Stock++ Keymap with remapped hold button | Unedited capture |
| [stockpp-display.png](screenshots/stockpp-display.png) | 2026-10-01 | Stock++ Display and LED policy controls | Unedited capture |
| [stockpp-volume.png](screenshots/stockpp-volume.png) | 2026-10-01 | Stock++ Volume settings | Unedited capture |
| [stockpp-themes.png](screenshots/stockpp-themes.png) | 2026-10-01 | Stock++ theme selection | Unedited capture |
| [stockpp-homepage.png](screenshots/stockpp-homepage.png) | 2026-10-01 | Stock++ Homepage settings | Unedited capture |
| [stockpp-system.png](screenshots/stockpp-system.png) | 2026-10-01 | Stock++ System settings | Unedited capture |
| [stockpp-osd-panel.png](screenshots/stockpp-osd-panel.png) | 2026-10-09 | Stock-style OSD panel; frontend underneath retained from style-switching session | Unedited capture |
| [stockpp-power-dialog.png](screenshots/stockpp-power-dialog.png) | 2026-10-13 | Stock-style OSD power confirmation | Unedited capture |
| [tmenu-stockpp.png](screenshots/tmenu-stockpp.png) | 2026-10-11 | Stock-style menu in RetroArch; game frame masked | Opaque game-content masks |
| [mainui-desktop.png](screenshots/mainui-desktop.png) | 2026-10-13 | Experimental BrickOS desktop | Unedited capture |
| [mainui-appearance.png](screenshots/mainui-appearance.png) | 2026-10-10 | Experimental BrickOS Appearance options | Unedited capture |
| [gameswitcher-cards.png](screenshots/gameswitcher-cards.png) | 2026-10-13 | GameSwitcher card; title and game frame masked | Opaque game-content masks |
| [uninstall-confirmation.png](screenshots/uninstall-confirmation.png) | 2026-10-11 | Uninstall confirmation; Cancel selected | Unedited capture |
| [stockpp-bluetooth-devices.png](screenshots/stockpp-bluetooth-devices.png) | 2026-10-18 | Bluetooth enabled; saved Redmi Buds 4 disconnected | Unedited capture |
| [stockpp-button-mapping.png](screenshots/stockpp-button-mapping.png) | 2026-10-18 | Active Nintendo/default Button Mapping profile | Unedited capture |
| [stockpp-led-custom.png](screenshots/stockpp-led-custom.png) | 2026-10-18 | Custom LED fields; LEDs off; Default policy restored afterwards | Unedited capture |
| [stockpp-themes-current.png](screenshots/stockpp-themes-current.png) | 2026-10-18 | Stock++ Themes with independent visual selectors | Unedited capture |
| [stockpp-bootlogo-preview.png](screenshots/stockpp-bootlogo-preview.png) | 2026-10-18 | Cyan circuit bootlogo preview; applied image unchanged | Unedited capture |
| [stockpp-loading-presets.png](screenshots/stockpp-loading-presets.png) | 2026-10-18 | RetroArch Pulse preview; retro-arcade remains selected; local presets visible | Unedited capture |
| [stockpp-settings-updates.png](screenshots/stockpp-settings-updates.png) | 2026-10-18 | Script-backed Updates tile with outline icon | Unedited capture |
| [stockpp-updates.png](screenshots/stockpp-updates.png) | 2026-10-18 | Updates JSON menu; release 0.1.2 and offline retry state | Unedited capture |
| [stockpp-component-versions.png](screenshots/stockpp-component-versions.png) | 2026-10-18 | Installed component versions submenu | Unedited capture |
| [stockpp-frontend-selector.png](screenshots/stockpp-frontend-selector.png) | 2026-10-18 | BrickOS focused while Stock++ remains active | Unedited capture |

Stock++ denotes BrickOS's enhanced stock-style presentation, rather than the
official TrimUI executable. The OSD panel image documents its stock-style
surface; the frontend underneath remains from the style-switching session.
The input and LED daemons run in the background. Screen captures document
settings and overlays; they cannot show the physical LEDs' emitted colours.
