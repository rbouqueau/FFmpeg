/*
 * DVB teletext common functions.
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef AVCODEC_DVBTXT_H
#define AVCODEC_DVBTXT_H

#include <stdint.h>

#include "libavutil/attributes.h"
#include "libavutil/common.h"

/**
 * @brief Encode a byte with odd parity
 * Bits have to be like this:
 * MSb -   -   -   -   -   -   LSb
 * 0   b7  b6  b5  b4  b3  b2  b1
 * 0x7F (max value)
 * @param byte byte to be encoded
 * @return uint8_t Encoded byte
 */
static av_always_inline uint8_t odd_parity_coding(uint8_t byte) {
    return (!av_parity(byte) << 7 | (0x7F & byte));
}

/**
 * @brief Encode a byte in hamming8/4
 * Bits have to be like this:
 * MSb -   -   -   -   -   -   LSb
 * 0   0   0   0   b4  b3  b2  b1
 * 0x0F (max value)
 * @param byte byte to be encoded
 * @return uint8_t Encoded byte
 */
static av_always_inline uint8_t hamming_8_4_coding(uint8_t byte) {
    uint8_t/*bool*/ D1 = 0x01 & byte;
    uint8_t/*bool*/ D2 = 0x01 & byte >> 1;
    uint8_t/*bool*/ D3 = 0x01 & byte >> 2;
    uint8_t/*bool*/ D4 = 0x01 & byte >> 3;

    uint8_t/*bool*/ P1 = 1 ^ D1 ^ D3 ^ D4; // ^ -> XOR in C
    uint8_t/*bool*/ P2 = 1 ^ D1 ^ D2 ^ D4;
    uint8_t/*bool*/ P3 = 1 ^ D1 ^ D2 ^ D3;
    uint8_t/*bool*/ P4 = 1 ^ P1 ^ D1 ^ P2 ^ D2 ^ P3 ^ D3 ^ D4;

    return (D4<<7 | P4<<6 | D3<<5 | P3<<4 | D2<<3 | P2<<2 | D1<<1 | P1);
}

/**
 * @brief Swap a byte
 * MSb becomes LSb and so on
 * @param byte Input byte
 * @return uint8_t Swapped byte
 */
static av_always_inline uint8_t swap_byte(uint8_t byte) {
    uint8_t temp = 0x00;
    for(int i = 0; i < 8; i++) {
        temp = temp << 1;
        temp |= (0x01 & byte);
        byte = byte >> 1;
    }
    return temp;
}

/* Returns true if data identifier matches a teletext stream according to EN
 * 301 775 section 4.4.2 */
static av_always_inline int ff_data_identifier_is_teletext(int data_identifier)
{
    return (data_identifier >= 0x10 && data_identifier <= 0x1F ||
            data_identifier >= 0x99 && data_identifier <= 0x9B);
}

/* Returns true if data unit id matches EBU teletext data according to
 * EN 301 775 section 4.4.2 */
static av_always_inline int ff_data_unit_id_is_teletext(int data_unit_id)
{
    return (data_unit_id == 0x02 || data_unit_id == 0x03);
}

#endif /* AVCODEC_DVBTXT_H */
