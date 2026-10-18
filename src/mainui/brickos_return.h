#ifndef BRICKOS_MAINUI_BRICKOS_RETURN_H
#define BRICKOS_MAINUI_BRICKOS_RETURN_H

#include <stddef.h>

#define MAINUI_BRICKOS_RETURN_PATH "/tmp/brickos-mainui-return.json"
#define MAINUI_BRICKOS_RETURN_TOKEN 128
#define MAINUI_BRICKOS_RETURN_DIGEST 65

int mainui_brickos_return_digest(const void *bytes, size_t length, char out[MAINUI_BRICKOS_RETURN_DIGEST]);
int mainui_brickos_return_publish(const char *path, const char *token, const char *digest);
int mainui_brickos_return_publish_view(const char *path, const char *token, const char *digest,
                                      const char *view_digest);
int mainui_brickos_return_validate(const char *path, const char *pending_token, const char *digest);
int mainui_brickos_return_validate_view(const char *path, const char *pending_token, const char *digest,
                                       const char *view_path);
void mainui_brickos_return_consume(const char *path);

#endif
