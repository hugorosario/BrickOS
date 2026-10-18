#ifndef BRICKOS_MAINUI_MUSIC_PICTURE_H
#define BRICKOS_MAINUI_MUSIC_PICTURE_H

// The idle music picture for the official OSD (docs/mainui.md): the stock picture at source in
// the BrickOS palette for an accent seed and background level, each pixel through
// brickos_recolor, written as a PNG at destination by way of a temporary file renamed into
// place. Returns 0, or -1 with nothing written at destination. Safe on a worker thread once
// IMG_Init has run.
int mainui_music_picture_write(const char *source, const char *destination, const unsigned char seed[3], double level);

#endif
