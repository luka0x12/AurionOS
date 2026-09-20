/*
 * Blaze Browser - Network Layer (offline stub)
 *
 * Network drivers were removed from this AurionOS build, so remote
 * fetch always fails closed. blaze_core renders its error page on any
 * nonzero return. Local file:// pages keep working.
*/

#include "blaze.h"

int blaze_fetch(const char *url, char **out_content, uint32_t *out_len) {
    (void)url;
    if (out_content) *out_content = 0;
    if (out_len) *out_len = 0;
    return -1;
}

int blaze_fetch_https(const char *host, const char *path, char **out_content, uint32_t *out_len) {
    (void)host;
    (void)path;
    if (out_content) *out_content = 0;
    if (out_len) *out_len = 0;
    return -1;
}
