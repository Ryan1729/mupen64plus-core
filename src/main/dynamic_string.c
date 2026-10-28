/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - dynamic_string.c                                                  *
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

#include "dynamic_string.h"

#include <stdlib.h>

dynamic_string_grow_outcome dynamic_string_ensure(dynamic_string* string, size_t min_capacity) {
    if (min_capacity == 0 || string->capacity >= min_capacity) {
        return DYNAMIC_STRING_SUCCESS;
    }
    
    size_t new_capacity = min_capacity;
    
    char *new_buffer = realloc(string->buffer, new_capacity);
    if (new_buffer == NULL) {
        free(string->buffer);
        string->capacity = 0;
        string->len = 0;

        return DYNAMIC_STRING_MEMORY_ERROR;
    }

    string->buffer = new_buffer;
    string->capacity = new_capacity;

    return DYNAMIC_STRING_SUCCESS;
}

dynamic_string_grow_outcome dynamic_string_grow(dynamic_string* string) {
    size_t new_capacity = string->capacity == 0 ? 16 : string->capacity * 2;

    return dynamic_string_ensure(string, new_capacity);
}

dynamic_string_grow_outcome dynamic_string_push(dynamic_string* string, char c) {
    size_t needed_capacity = 1;

    dynamic_string_grow_outcome outcome = dynamic_string_ensure(string, string->len + needed_capacity);

    if (outcome == DYNAMIC_STRING_SUCCESS) {
        string->buffer[string->len] = c;
        string->len += needed_capacity;
    }

    return outcome;
}

void dynamic_string_free(dynamic_string* string) {
    if (string != NULL) {
        free(string->buffer);
        string->capacity = 0;
        string->len = 0;
    }
}
