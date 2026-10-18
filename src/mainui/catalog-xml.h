#ifndef BRICKOS_MAINUI_CATALOG_XML_H
#define BRICKOS_MAINUI_CATALOG_XML_H

#include "catalog.h"

// Read stock gameList rows, before sorting. A non-positive result selects the FS fallback.
int mainui_catalog_xml(const struct mainui_fs *fs, const struct mainui_system *system,
                       const struct mainui_names *names, const char *language, struct mainui_game **games);

#endif
