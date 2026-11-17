/* SPDX-License-Identifier: MPL-2.0 */
#ifndef MR_WS_H
#define MR_WS_H

#include <stdint.h>

/* Sec-WebSocket-Accept for a Sec-WebSocket-Key (28 characters and a NUL) */
void mr_ws_accept(const char *key, char out[32]);

#endif
