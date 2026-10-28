/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - memory_slice.h                                                  *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2026 Ryan1729                                           *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include "memory_slice.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

//#define MEM_SLICE_NO_BYTESWAP

unsigned char memory_slice_read_8(memory_slice slice, uint32_t addr) {
    uint32_t word = *(uint32_t*)(slice.base + (addr & 0xfffffffc));
#ifdef MEM_SLICE_NO_BYTESWAP
    return word;
#else
    return (word >> ((3 - (addr & 3)) * 8)) & 0xFF;
#endif
}

void memory_slice_write_8(memory_slice slice, uint32_t addr, unsigned char payload) {
    uint32_t word;
    uint32_t mask;

#ifdef MEM_SLICE_NO_BYTESWAP
    mask = 0xFF << ((addr & 3) * 8);
    word = ((uint32_t)payload) << ((addr & 3) * 8);
#else
    mask = 0xFF << ((3 - (addr & 3)) * 8);
    word = ((uint32_t)payload) << ((3 - (addr & 3)) * 8);
#endif
    uint32_t *base = (uint32_t*)(slice.base + (addr & 0xfffffffc));

    *base = (*base & ~mask) | (word & mask);
}
