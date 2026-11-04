// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
inline int16_t decodeMulaw(uint8_t encoded){
    uint8_t code=~encoded;int magnitude=((code&15)<<3)+132;
    magnitude<<=(code>>4)&7;magnitude-=132;
    return (code&128)?-magnitude:magnitude;
}
