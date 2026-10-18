#ifndef BRICKOS_MAINUI_MARKERS_H
#define BRICKOS_MAINUI_MARKERS_H

// The run markers the frontend wrapper reads: <run>/<token>.ready after the first rendered
// frame and <token>.done after a clean end (launch handoff, time limit, quit), written
// atomically. The wrapper passes the directory and a new token for every run in
// BRICKOS_MAINUI_RUN and BRICKOS_MAINUI_TOKEN; without them (a manual or diagnostic run) no
// marker is written. A run that is killed or crashes never writes .done.

enum mainui_marker { MAINUI_MARKER_READY, MAINUI_MARKER_DONE };

// Writes a marker from the environment's run directory and token. Returns 1 when written, 0
// when the environment names no run, -1 on a write failure.
int mainui_marker_write(enum mainui_marker marker);

// The same with an explicit directory and token, for tests.
int mainui_marker_write_to(const char *run, const char *token, enum mainui_marker marker);

#endif
