#ifndef BRICKOS_MAINUI_PRESENTATION_H
#define BRICKOS_MAINUI_PRESENTATION_H

#include "model.h"

// Parses one explicit frontend presentation name. Returns 0 on success, -1 otherwise.
int mainui_presentation_parse(const char *name, enum mainui_presentation *out);
const char *mainui_presentation_name(enum mainui_presentation presentation);

#endif
