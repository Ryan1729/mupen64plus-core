/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - dynamic_string.h                                                  *
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

#ifndef __DYNAMIC_STRING_H__
#define __DYNAMIC_STRING_H__

#include <stddef.h>

typedef struct {
    char *buffer;
    size_t len;
    size_t capacity;
} dynamic_string;

typedef enum {
    DYNAMIC_STRING_SUCCESS,
    DYNAMIC_STRING_MEMORY_ERROR,
} dynamic_string_grow_outcome;

dynamic_string_grow_outcome dynamic_string_ensure(dynamic_string* string, size_t min_capacity);

dynamic_string_grow_outcome dynamic_string_grow(dynamic_string* string);

dynamic_string_grow_outcome dynamic_string_push(dynamic_string* string, char c);

void dynamic_string_free(dynamic_string* string);

#endif
