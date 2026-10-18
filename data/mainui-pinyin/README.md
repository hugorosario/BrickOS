# Pinyin source attribution

`src/mainui/pinyin-data.h` derives from `kMandarin` in Unicode 5.2's
`Unihan_Readings.txt`, copyright 1991-2009 Unicode, Inc., under the included
[Unicode license](LICENSE.txt).

Upstream archive: https://www.unicode.org/Public/5.2.0/ucd/Unihan.zip.
The readings input has SHA-256
`ce3c454671fe90f367617d2f5f76333e68e0925cb93b026a294da12ea422778e`.

The table is already generated and included in the C source. Compilation needs
no download or generation step for it. Its compatibility choices preserve the
observed stock initials and ROM ordering, including differing first readings,
coverage and unsupported characters. No stock firmware table is bundled.
