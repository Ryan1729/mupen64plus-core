/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - archipelago_bizhawk.c                                               *
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

#include "api/callbacks.h"
#include "archipelago_bizhawk.h"
#include "base64.h"
#include "cJSON.h"
#include "dynamic_string.h"
#include "memory_slice.h"
#include "main.h"
#include "rom.h"
#include "version.h"

#include <assert.h>
#include <SDL.h>
#include <SDL_net.h>
#include <SDL_timer.h>
#include <stdlib.h>
#include <string.h>

#ifndef NDEBUG
#define assert_always_run(expression) assert(expression)
#else
#define assert_always_run(expression) expression
#endif

// Shows debug messages tagged with M64MSG_VERBOSE, so verbose messages
// need to be enabled for this to do anything.
// PERF Could avoid formatting the strings when verbose messages are
//      disabled by passing verbose flag to this module's init.
#define SHOW_READ_MESSAGES

const Uint32 TICK_INTERVAL_MS = 125;
const Uint32 SERVER_ATTEMPT_INTERVAL_MS = 1000;
const Uint16 SOCKET_PORT_FIRST = 43055;
const Uint16 SOCKET_PORT_LAST = 43060;
const int CHUNK_SIZE = 1;
const int BUFFER_SIZE = 1024;
const int MAX_MEM_SIZE = RDRAM_MAX_SIZE;

// Returns 1 unless there is an error, such as the obj
// not having the given field, etc., in which case 0 is returned
int get_int(const cJSON *obj, const char* key, int* output) {
    cJSON *value = cJSON_GetObjectItemCaseSensitive(obj, key);

    if (cJSON_IsNumber(value)) {
        *output = value->valueint;
        return 1;
    }

    return 0;
}

typedef enum {
    ARCHIPELAGO_BIZHAWK_WAITING,
    ARCHIPELAGO_BIZHAWK_DISCONNECTED,
    ARCHIPELAGO_BIZHAWK_ACCEPTING_REQUESTS,
    ARCHIPELAGO_BIZHAWK_CHECKING_MESSAGES,
} archipelago_bizhawk_state;

static archipelago_bizhawk_state l_state = ARCHIPELAGO_BIZHAWK_WAITING;
static SDL_TimerID l_timer_id = 0;
static Uint32 l_time_since_last_server_attempt_ms = SERVER_ATTEMPT_INTERVAL_MS;
static TCPsocket l_server, l_client;
static SDLNet_SocketSet l_socket_set;
static dynamic_string l_input_buffer = {};
static dynamic_string l_output_buffer = {};

typedef enum {
    PROCESS_MESSAGE_SUCCESS = 0,
    PROCESS_MESSAGE_NO_MEMORY,
    // TODO replace this with sending an error back down the connection
    // in every case.
    PROCESS_MESSAGE_BAD_INPUT,
} process_message_outcome;

process_message_outcome build_basic_response(cJSON *output_obj, char *type) {
    assert(output_obj != NULL);

    cJSON *output_type = cJSON_CreateString(type);
    if (output_type != NULL && cJSON_AddItemToObjectCS(output_obj, "type", output_type)) {
        return PROCESS_MESSAGE_SUCCESS;
    }

    return PROCESS_MESSAGE_NO_MEMORY;
}

process_message_outcome build_error_response(cJSON *output_obj, const char *fmt, ...) {
    assert(output_obj != NULL);

    cJSON *output_type = cJSON_CreateString("ERROR");
    if (output_type != NULL && cJSON_AddItemToObjectCS(output_obj, "type", output_type)) {
        va_list ap;
        char error_message[1024];

        va_start(ap, fmt);
        vsnprintf(error_message, 1024, fmt, ap);
        error_message[1023] = 0;
        va_end(ap);

        cJSON *output_err = cJSON_CreateString(error_message);

        if (output_err != NULL && cJSON_AddItemToObjectCS(output_obj, "err", output_err)) {
            return PROCESS_MESSAGE_SUCCESS;
        }
    }

    return PROCESS_MESSAGE_NO_MEMORY;
}

process_message_outcome handle_ping(cJSON *output_obj) {
    return build_basic_response(output_obj, "PONG");
}

process_message_outcome build_read_response(cJSON *output_obj, unsigned char* data, int data_len) {
    assert(output_obj != NULL);

    cJSON *output_type = cJSON_CreateString("READ_RESPONSE");
    if (output_type == NULL) {
        return PROCESS_MESSAGE_NO_MEMORY;
    }

    if (!cJSON_AddItemToObjectCS(output_obj, "type", output_type)) {
        return PROCESS_MESSAGE_NO_MEMORY;
    }

    // Don't care about this, but have to pass it.
    int flen;
    char* encoded = base64(data, data_len, &flen);
    if (encoded == NULL) {
        return PROCESS_MESSAGE_NO_MEMORY;
    }

    cJSON *output_value = cJSON_CreateString(encoded);
    free(encoded);
    if (output_value == NULL) {
        return PROCESS_MESSAGE_NO_MEMORY;
    }

    if (!cJSON_AddItemToObjectCS(output_obj, "value", output_value)) {
        return PROCESS_MESSAGE_NO_MEMORY;
    }

    return PROCESS_MESSAGE_SUCCESS;
}

process_message_outcome build_guard_response(cJSON *output_obj, int validated, int address) {
    assert(output_obj != NULL);

    cJSON *output_type = cJSON_CreateString("GUARD_RESPONSE");
    if (output_type == NULL) {
        return PROCESS_MESSAGE_NO_MEMORY;
    }

    if (!cJSON_AddItemToObjectCS(output_obj, "type", output_type)) {
        return PROCESS_MESSAGE_NO_MEMORY;
    }

    cJSON *output_validated = cJSON_CreateNumber(validated);
    if (output_validated == NULL) {
        return PROCESS_MESSAGE_NO_MEMORY;
    }

    if (!cJSON_AddItemToObjectCS(output_obj, "validated", output_validated)) {
        return PROCESS_MESSAGE_NO_MEMORY;
    }

    cJSON *output_address = cJSON_CreateNumber(address);
    if (output_address == NULL) {
        return PROCESS_MESSAGE_NO_MEMORY;
    }

    if (!cJSON_AddItemToObjectCS(output_obj, "address", output_address)) {
        return PROCESS_MESSAGE_NO_MEMORY;
    }

    return PROCESS_MESSAGE_SUCCESS;
}

typedef enum {
    DOMAIN_INVALID,
    DOMAIN_RDRAM,
    DOMAIN_ROM,
} parsed_domain;

static const parsed_domain FIRST_VALID_DOMAIN = DOMAIN_RDRAM;
static const parsed_domain LAST_VALID_DOMAIN = DOMAIN_ROM;

parsed_domain check_domain(const cJSON *request) {
    char *domain = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "domain"));

    if (domain != NULL) {
        if (strcmp(domain, "RDRAM") == 0) {
            return DOMAIN_RDRAM;
        } else if (strcmp(domain, "ROM") == 0) {
            return DOMAIN_ROM;
        }
    }

    return DOMAIN_INVALID;
}

typedef enum {
    MEMORY_SLICE_SUCCESS,
    MEMORY_SLICE_UNKNOWN_INPUT,
    MEMORY_SLICE_NOT_READY,
    MEMORY_SLICE_BAD_INPUT,
} memory_slice_outcome;

memory_slice_outcome get_memory_slice(memory_slice *slice, parsed_domain domain)
{
    switch (domain)
    {
        case DOMAIN_RDRAM: {
            struct device* dev = &g_dev;

            slice->base = (unsigned char *) dev->rdram.dram;
            slice->size = dev->rdram.dram_size;
            break;
        }
        case DOMAIN_ROM: {
            struct device* dev = &g_dev;
            struct cart_rom* cart_rom = &dev->cart.cart_rom;

            slice->base = (unsigned char *) cart_rom->rom;
            slice->size = cart_rom->rom_size;
            break;
        };
        case DOMAIN_INVALID:
            return MEMORY_SLICE_BAD_INPUT;
        default:
            return MEMORY_SLICE_UNKNOWN_INPUT;
    }

    if (slice->size == 0) {
        return MEMORY_SLICE_NOT_READY;
    }

    return MEMORY_SLICE_SUCCESS;
}

// This is a remnant of when we thought that conversions like
// `address ^ 3` woudl be needed, reading the bizhawk source code. Seems
// best to leave this here as a placeholder until we implement all the
// memory domains we can, just in case.
process_message_outcome convert_address(int *base_address, parsed_domain domain, int address) {
    switch (domain) {
        case DOMAIN_RDRAM:
            *base_address = address;
            return PROCESS_MESSAGE_SUCCESS;
        case DOMAIN_ROM:
            *base_address = address;
            return PROCESS_MESSAGE_SUCCESS;
        case DOMAIN_INVALID:
            DebugMessage(M64MSG_ERROR, "Invalid domain in convert_address()");
            return PROCESS_MESSAGE_BAD_INPUT;
        default:
            DebugMessage(M64MSG_ERROR, "Unknown domain in convert_address()");
            return PROCESS_MESSAGE_BAD_INPUT;
    }
}

process_message_outcome handle_read_with_data(unsigned char *data, int requested_size, parsed_domain domain, memory_slice slice, cJSON *output_obj, const cJSON *request) {
    assert(output_obj != NULL);

    int address;
    if (!get_int(request, "address", &address)) {
        return build_error_response(output_obj, "Could not read address field.");
    }

    int base_address;
    process_message_outcome outcome = convert_address(&base_address, domain, address);
    if (outcome != PROCESS_MESSAGE_SUCCESS) {
        return outcome;
    }

    if (base_address + requested_size <= slice.size) {
        for (int i = 0; i < requested_size; ++i) {
            data[i] = memory_slice_read_8(slice, base_address + i);
        }

        return build_read_response(output_obj, data, requested_size);
    } else {
        // We get read requests for all n64 apworlds the user has
        // installed at the begining to identify the rom, in an
        // unspecified order. This may include ones that expect a larger
        // rom than the user has loaded currently, making it hit this
        // case. So when reading the rom, return spurious zeroes, to
        // avoid the user needing to uninstall unrelated apworlds to the
        // one they want to play, just for the bizhawk client to not
        // crash out.
        if (domain == DOMAIN_ROM) {
            for (int i = 0; i < requested_size; ++i) {
                data[i] = 0;
            }

            return build_read_response(output_obj, data, requested_size);
        }

        return build_error_response(output_obj, "Requested read is past the end of the memory domain.");
    }
}

process_message_outcome handle_read(cJSON *output_obj, const cJSON *request) {
    assert(output_obj != NULL);

    parsed_domain domain = check_domain(request);
    if (domain == DOMAIN_INVALID) {
        return PROCESS_MESSAGE_BAD_INPUT;
    }

    memory_slice slice;
    memory_slice_outcome slice_outcome = get_memory_slice(&slice, domain);
    switch (slice_outcome) {
        case MEMORY_SLICE_SUCCESS:
            // All good; keep going.
        break;
        case MEMORY_SLICE_NOT_READY:
            return build_error_response(output_obj, "Game was not loaded yet.");
        break;
        case MEMORY_SLICE_BAD_INPUT:
            return build_error_response(output_obj, "Invalid memory domain was requested.");
        break;
        default:
            return build_error_response(output_obj, "Unknown memory domain was requested.");
        break;
    }

    int requested_size;
    if (!get_int(request, "size", &requested_size)) {
        return build_error_response(output_obj, "Could not read size field.");
    }
    if (requested_size <= 0) {
        return build_error_response(output_obj, "Got negative size: %d.", requested_size);
    }
    if (requested_size > slice.size) {
        return build_error_response(output_obj, "Requested size of %d exceeded expected size of %d.", requested_size, slice.size);
    }

    unsigned char *data = malloc(requested_size);
    if (data == NULL) {
        return PROCESS_MESSAGE_NO_MEMORY;
    }
    process_message_outcome outcome = handle_read_with_data(data, requested_size, domain, slice, output_obj, request);

    free(data);

    return outcome;
}

process_message_outcome handle_guard_with_expected_data(
    cJSON *output_obj,
    const cJSON *request,
    const unsigned char *expected_data,
    int expected_data_len
) {
    parsed_domain domain = check_domain(request);
    if (domain == DOMAIN_INVALID) {
        return PROCESS_MESSAGE_BAD_INPUT;
    }

    memory_slice slice;
    memory_slice_outcome slice_outcome = get_memory_slice(&slice, domain);
    switch (slice_outcome) {
        case MEMORY_SLICE_SUCCESS:
            // All good; keep going.
        break;
        case MEMORY_SLICE_NOT_READY:
            return build_error_response(output_obj, "Game was not loaded yet.");
        break;
        case MEMORY_SLICE_BAD_INPUT:
            return build_error_response(output_obj, "Invalid memory domain was requested.");
        break;
        default:
            return build_error_response(output_obj, "Unknown memory domain was requested.");
        break;
    }

    if (expected_data_len <= 0 || expected_data_len > slice.size) {
        return PROCESS_MESSAGE_BAD_INPUT;
    }

    int address;
    if (!get_int(request, "address", &address)) {
        return PROCESS_MESSAGE_BAD_INPUT;
    }

    unsigned char *data = malloc(expected_data_len);
    if (data == NULL) {
        return PROCESS_MESSAGE_NO_MEMORY;
    }

    int base_address;
    process_message_outcome outcome = convert_address(&base_address, domain, address);
    if (outcome != PROCESS_MESSAGE_SUCCESS) {
        return outcome;
    }

    if (base_address + expected_data_len <= slice.size) {
        for (int i = 0; i < expected_data_len; ++i) {
            data[i] = memory_slice_read_8(slice, base_address + i);
        }

        int validated = 1;
        for (int i = 0; i < expected_data_len; ++i) {
            if (expected_data[i] != data[i]) {
                validated = 0;
                break;
            }
        }

        outcome = build_guard_response(output_obj, validated, address);
    } else {
        outcome = PROCESS_MESSAGE_BAD_INPUT;
    }

    free(data);

    return outcome;
}

process_message_outcome handle_guard(cJSON *output_obj, const cJSON *request) {
    assert(output_obj != NULL);

    char *expected_data_encoded = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "expected_data"));
    if (expected_data_encoded == NULL) {
        return PROCESS_MESSAGE_BAD_INPUT;
    }

    size_t expected_data_encoded_len = strlen(expected_data_encoded);

    if (expected_data_encoded_len < 0 || expected_data_encoded_len > MAX_MEM_SIZE) {
        return PROCESS_MESSAGE_BAD_INPUT;
    }

    int expected_data_len = 0;
    unsigned char* expected_data = unbase64(expected_data_encoded, expected_data_encoded_len, &expected_data_len);
    if (expected_data == NULL) {
        return PROCESS_MESSAGE_NO_MEMORY;
    }

    process_message_outcome outcome = handle_guard_with_expected_data(output_obj, request, expected_data, expected_data_len);

    free(expected_data);

    return outcome;
}

process_message_outcome handle_write(cJSON *output_obj, const cJSON *request) {
    assert(output_obj != NULL);

    parsed_domain domain = check_domain(request);
    if (domain == DOMAIN_INVALID || domain == DOMAIN_ROM) {
        return build_error_response(output_obj, "Bad/missing domain field: %d", domain);
    }

    int address;
    if (!get_int(request, "address", &address)) {
        return build_error_response(output_obj, "Bad/missing address field");
    }

    char *value_encoded = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "value"));
    if (value_encoded == NULL) {
        return build_error_response(output_obj, "Bad/missing value field");
    }

    size_t value_encoded_len = strlen(value_encoded);

    if (value_encoded_len < 0 || value_encoded_len > MAX_MEM_SIZE) {
        return PROCESS_MESSAGE_BAD_INPUT;
    }

    int payload_len = 0;
    unsigned char* payload = unbase64(value_encoded, value_encoded_len, &payload_len);
    if (payload == NULL) {
        return PROCESS_MESSAGE_NO_MEMORY;
    }

    int base_address;
    process_message_outcome outcome = convert_address(&base_address, domain, address);
    if (outcome != PROCESS_MESSAGE_SUCCESS) {
        return outcome;
    }

    memory_slice slice;
    memory_slice_outcome slice_outcome = get_memory_slice(&slice, domain);
    switch (slice_outcome) {
        case MEMORY_SLICE_SUCCESS:
            // All good; keep going.
        break;
        case MEMORY_SLICE_NOT_READY:
            return build_error_response(output_obj, "Game was not loaded yet.");
        break;
        case MEMORY_SLICE_BAD_INPUT:
            return build_error_response(output_obj, "Invalid memory domain was requested.");
        break;
        default:
            return build_error_response(output_obj, "Unknown memory domain was requested.");
        break;
    }

    if (base_address + payload_len <= slice.size) {
        for (int i = 0; i < payload_len; ++i) {
            memory_slice_write_8(slice, base_address + i, payload[i]);
        }

        outcome = build_basic_response(output_obj, "WRITE_RESPONSE");
    } else {
        outcome = PROCESS_MESSAGE_BAD_INPUT;
    }

    free(payload);

    return outcome;
}

process_message_outcome handle_hash(cJSON *output_obj, const cJSON *request) {
    assert(output_obj != NULL);

    cJSON *output_type = cJSON_CreateString("HASH_RESPONSE");
    if (output_type == NULL) {
        return PROCESS_MESSAGE_NO_MEMORY;
    }

    if (!cJSON_AddItemToObjectCS(output_obj, "type", output_type)) {
        return PROCESS_MESSAGE_NO_MEMORY;
    }

    // Don't care about this, but have to pass it.
    int flen;
    // This does not need to be any particular hash. But, it is meant to
    // change when, (and only when) the rom changes, to detect that
    // change. This makes the ROM_SETTINGS.MD5 value a perfect fit.
    char* encoded = base64(ROM_SETTINGS.MD5, 32, &flen);
    if (encoded == NULL) {
        return PROCESS_MESSAGE_NO_MEMORY;
    }

    cJSON *output_value = cJSON_CreateString(encoded);
    free(encoded);
    if (output_value == NULL) {
        return PROCESS_MESSAGE_NO_MEMORY;
    }

    if (!cJSON_AddItemToObjectCS(output_obj, "value", output_value)) {
        return PROCESS_MESSAGE_NO_MEMORY;
    }

    return PROCESS_MESSAGE_SUCCESS;
}

process_message_outcome handle_system(cJSON *output_obj, const cJSON *request) {
    assert(output_obj != NULL);

    cJSON *output_type = cJSON_CreateString("SYSTEM_RESPONSE");
    if (output_type == NULL) {
        return PROCESS_MESSAGE_NO_MEMORY;
    }

    if (!cJSON_AddItemToObjectCS(output_obj, "type", output_type)) {
        return PROCESS_MESSAGE_NO_MEMORY;
    }

    cJSON *output_value = cJSON_CreateString("N64");
    if (output_value == NULL) {
        return PROCESS_MESSAGE_NO_MEMORY;
    }

    if (!cJSON_AddItemToObjectCS(output_obj, "value", output_value)) {
        return PROCESS_MESSAGE_NO_MEMORY;
    }

    return PROCESS_MESSAGE_SUCCESS;
}

process_message_outcome handle_memory_size(cJSON *output_obj, const cJSON *request) {
    assert(output_obj != NULL);

    parsed_domain domain = check_domain(request);
    if (domain == DOMAIN_INVALID) {
        return build_error_response(output_obj, "Bad/missing domain field: %d", domain);
    }

    cJSON *output_type = cJSON_CreateString("MEMORY_SIZE_RESPONSE");
    if (output_type == NULL) {
        return PROCESS_MESSAGE_NO_MEMORY;
    }

    if (!cJSON_AddItemToObjectCS(output_obj, "type", output_type)) {
        return PROCESS_MESSAGE_NO_MEMORY;
    }

    memory_slice slice;
    memory_slice_outcome slice_outcome = get_memory_slice(&slice, domain);
    switch (slice_outcome) {
        case MEMORY_SLICE_SUCCESS:
            // All good; keep going.
        break;
        case MEMORY_SLICE_NOT_READY:
            return build_error_response(output_obj, "Game was not loaded yet.");
        break;
        case MEMORY_SLICE_BAD_INPUT:
            return build_error_response(output_obj, "Invalid memory domain was requested.");
        break;
        default:
            return build_error_response(output_obj, "Unknown memory domain was requested.");
        break;
    }

    cJSON *output_value = cJSON_CreateNumber(slice.size);
    if (output_value == NULL) {
        return PROCESS_MESSAGE_NO_MEMORY;
    }

    if (!cJSON_AddItemToObjectCS(output_obj, "value", output_value)) {
        return PROCESS_MESSAGE_NO_MEMORY;
    }

    return PROCESS_MESSAGE_SUCCESS;
}

process_message_outcome handle_unknown_command(cJSON *output_obj, const char *type) {
    assert(output_obj != NULL);

    return build_error_response(output_obj, "Unknown command: %s", type);
}

process_message_outcome process_message(dynamic_string* output, const dynamic_string* input) {
    // Messages look like either:
    // * "VERSION\n"
    // * A JSON array of objects, followed by a newline.

    if (strncmp(input->buffer, "VERSION\n", 8) == 0) {
        int used_length = snprintf(
            NULL,
            0,
            "%d\n",
            ARCHIPELAGO_BIZHAWK_API_VERSION
        );
        // Need to have room for nul terminator, or else one is stamped
        // on our last byte.
        int needed_capacity = used_length + 1;

        dynamic_string_ensure(output, output->len + needed_capacity);

        snprintf(
            output->buffer,
            needed_capacity,
            "%d\n",
            ARCHIPELAGO_BIZHAWK_API_VERSION
        );

        output->len += used_length;

        return PROCESS_MESSAGE_SUCCESS;
    }

    cJSON *input_array = cJSON_ParseWithLength(input->buffer, input->len);
    if (input_array == NULL) {
        return PROCESS_MESSAGE_BAD_INPUT;
    }
    int input_array_len = cJSON_GetArraySize(input_array);

    cJSON *output_array = cJSON_CreateArray();

    process_message_outcome outcome = PROCESS_MESSAGE_SUCCESS;
    int previously_failed_guard = 0;
    int previously_failed_guard_address = 0;

#ifdef SHOW_READ_MESSAGES
    int was_read = 0;
#endif

    for (int i = 0; i < input_array_len; ++i) {
        if (previously_failed_guard) {
            cJSON *output_obj = cJSON_CreateObject();

            if (output_obj != NULL) {
                outcome = build_guard_response(output_obj, 0, previously_failed_guard_address);

                if (outcome == PROCESS_MESSAGE_SUCCESS) {
                    assert_always_run(cJSON_AddItemToArray(output_array, output_obj));
                }
            } else {
                outcome = PROCESS_MESSAGE_NO_MEMORY;
            }

            if (outcome != PROCESS_MESSAGE_SUCCESS) {
                cJSON_Delete(output_obj);
                break;
            }
        }

        cJSON *input_obj = cJSON_GetArrayItem(input_array, i);
        if (input_obj == NULL) {
            outcome = PROCESS_MESSAGE_BAD_INPUT;
            break;
        }

        const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(input_obj, "type"));
        if (type == NULL) {
            outcome = PROCESS_MESSAGE_BAD_INPUT;
            break;
        }

        cJSON *output_obj = cJSON_CreateObject();
        if (output_obj != NULL) {
            assert_always_run(cJSON_AddItemToArray(output_array, output_obj));

            if (strcmp(type, "PING") == 0) {
                outcome = handle_ping(output_obj);
            } else if (strcmp(type, "READ") == 0) {
#ifdef SHOW_READ_MESSAGES
                was_read = 1;
                DebugMessage(M64MSG_VERBOSE, "%.*s", (int)l_input_buffer.len, l_input_buffer.buffer);
#endif
                outcome = handle_read(output_obj, input_obj);
            } else if (strcmp(type, "GUARD") == 0) {
                outcome = handle_guard(output_obj, input_obj);
                if (outcome != PROCESS_MESSAGE_SUCCESS) {
                    previously_failed_guard = 1;

                    assert_always_run(get_int(output_obj, "address", &previously_failed_guard_address));
                }
            } else if (strcmp(type, "WRITE") == 0) {
                outcome = handle_write(output_obj, input_obj);

            } else if (strcmp(type, "HASH") == 0) {
                outcome = handle_hash(output_obj, input_obj);
            } else if (strcmp(type, "SYSTEM") == 0) {
                outcome = handle_system(output_obj, input_obj);
            } else if (strcmp(type, "MEMORY_SIZE") == 0) {
#ifdef SHOW_READ_MESSAGES
                // These are effectively a read, just of the length of
                // the domain
                was_read = 1;
                DebugMessage(M64MSG_VERBOSE, "%.*s", (int)l_input_buffer.len, l_input_buffer.buffer);
#endif
                outcome = handle_memory_size(output_obj, input_obj);
            } else {
                outcome = handle_unknown_command(output_obj, type);
            }
        } else {
            outcome = PROCESS_MESSAGE_NO_MEMORY;
            break;
        }
    }

    if (outcome == PROCESS_MESSAGE_SUCCESS) {
        // cJSON's README says:
        // "Note that you should provide 5 bytes more than is actually
        // needed, because cJSON is not 100% accurate in estimating if
        // the provided memory is enough."
        while (!cJSON_PrintPreallocated(output_array, output->buffer, output->capacity - 5, 0)) {
            if (dynamic_string_grow(output) != DYNAMIC_STRING_SUCCESS) {
                outcome = PROCESS_MESSAGE_NO_MEMORY;
                break;
            }
        }
        output->len = strlen(output->buffer);

        if (outcome == PROCESS_MESSAGE_SUCCESS) {
            if (dynamic_string_push(output, '\n') != DYNAMIC_STRING_SUCCESS) {
                outcome = PROCESS_MESSAGE_NO_MEMORY;
            }
        }
    }

#ifdef SHOW_READ_MESSAGES
    if (was_read) {
        DebugMessage(M64MSG_VERBOSE, "%.*s", (int)l_output_buffer.len, l_output_buffer.buffer);
    }
#endif

    cJSON_Delete(output_array);
    cJSON_Delete(input_array);

    return outcome;
}

Uint32 archipelago_bizhawk_tick(Uint32 interval, void* param)
{
    l_time_since_last_server_attempt_ms += interval;

    switch (l_state) {
        case ARCHIPELAGO_BIZHAWK_WAITING: {
            int all_ready = 1;
            for (parsed_domain domain = FIRST_VALID_DOMAIN; domain <= LAST_VALID_DOMAIN; ++domain) {
                memory_slice slice;
                memory_slice_outcome slice_outcome = get_memory_slice(&slice, domain);
                switch (slice_outcome) {
                    case MEMORY_SLICE_SUCCESS:
                        // All good; keep going.
                    break;
                    case MEMORY_SLICE_NOT_READY:
                        all_ready = 0;
                    break;
                    case MEMORY_SLICE_BAD_INPUT:
                        all_ready = 0;
                        DebugMessage(M64MSG_ERROR, "Invalid domain requested during init checking");
                    break;
                    default:
                        all_ready = 0;
                        DebugMessage(M64MSG_ERROR, "Unknown domain requested during init checking");
                    break;
                }

                if (!all_ready) {
                    break;
                }
            }

            if (all_ready) {
                DebugMessage(M64MSG_INFO, "Archipelago BizHawk: Starting server");
                l_state = ARCHIPELAGO_BIZHAWK_DISCONNECTED;
            }
            break;
        }
        case ARCHIPELAGO_BIZHAWK_DISCONNECTED: {
            if (l_time_since_last_server_attempt_ms >= SERVER_ATTEMPT_INTERVAL_MS) {
                l_time_since_last_server_attempt_ms = 0;
                IPaddress ip;

                Uint16 port = SOCKET_PORT_FIRST;

                while (port <= SOCKET_PORT_LAST) {
                    if (SDLNet_ResolveHost(&ip, NULL, port) == -1) {
                        DebugMessage(M64MSG_ERROR, "Archipelago BizHawk: SDLNet_ResolveHost: %s", SDLNet_GetError());
                        continue;
                    }
                    l_server = SDLNet_TCP_Open(&ip);
                    if (l_server) {
                        break;
                    }

                    port += 1;
                }

                if (l_server) {
                    DebugMessage(M64MSG_INFO, "Archipelago BizHawk: Now accepting requests");
                    l_state = ARCHIPELAGO_BIZHAWK_ACCEPTING_REQUESTS;
                } else {
                    DebugMessage(M64MSG_ERROR, "Archipelago BizHawk: SDLNet_TCP_Open: %s", SDLNet_GetError());

                    // Try again next tick.
                }
            }
            break;
        }
        case ARCHIPELAGO_BIZHAWK_ACCEPTING_REQUESTS: {
            // Attempt to connect
            l_client = SDLNet_TCP_Accept(l_server);
            if (l_client) {
                // We need to put the socket in a set so we can call
                // SDLNet_CheckSockets on the set later, in order to
                // call SDLNet_SocketReady, to check for messages,
                // without blocking.
                l_socket_set = SDLNet_AllocSocketSet(1);
                if (l_socket_set) {
                    if (SDLNet_TCP_AddSocket(l_socket_set, l_client) == 1) {
                        // We will actually check for messages on the
                        // next tick.
                        DebugMessage(M64MSG_INFO, "Archipelago BizHawk: Now checking recieved messages");
                        l_state = ARCHIPELAGO_BIZHAWK_CHECKING_MESSAGES;
                    } else {
                        DebugMessage(M64MSG_ERROR, "Archipelago BizHawk: SDLNet_TCP_AddSocket: %s", SDLNet_GetError());

                        // Try again next tick.
                    }
                } else {
                    DebugMessage(M64MSG_ERROR, "Archipelago BizHawk: SDLNet_AllocSocketSet: %s", SDLNet_GetError());

                    // Try again next tick.
                }
            } else {
                // Not an unexpected state, the client may simply not be
                // started up yet. Try again next tick.
            }
            break;
        }
        case ARCHIPELAGO_BIZHAWK_CHECKING_MESSAGES: {
            // Check for a message on the connection, without waiting.
            int ready_count = SDLNet_CheckSockets(l_socket_set, 0);
            if (ready_count == 0) {
                // Check again next tick.
            } else if (ready_count >= 1) {
                l_input_buffer.len = 0;

                // We set it to something that is not an error, to
                // distinguish a bug where we never touch this, from
                // real errors.
                int last_recv_output = 1;
                while (1) {
                    char output_byte = 0;

                    // The documentation for SDLNet_TCP_Recv says that
                    // if we read more than is sent from the other end
                    // then the function will wait until that much is
                    // sent. The documentation then goes on to say that
                    // 1 byte at a time will be needed to be read for
                    // some applications, giving a text based example.
                    //
                    // We're reading JSON blobs here, so we need to read
                    // the whole message before we can parse it. We want
                    // to minimize the delay that we introduce into the
                    // system. Ideally we do all this inside of the
                    // leftover time for one frame.
                    //
                    // Hopefully reading like this 1 byte at a time
                    // avoids, or mostly avoids blocking, and usually
                    // just passes us the bytes from a local buffer,
                    // making this all work at a reasonable speed.
                    last_recv_output = SDLNet_TCP_Recv(l_client, &output_byte, 1);
                    if (last_recv_output <= 0) {
                        l_input_buffer.len = 0;
                        break;
                    }

                    if (l_input_buffer.capacity - l_input_buffer.len <= 0) {
                        // We don't have enough room, allocate a fair
                        // amount more, to avoid having O(n) allocations
                        if (dynamic_string_grow(&l_input_buffer) == DYNAMIC_STRING_MEMORY_ERROR) {
                            DebugMessage(M64MSG_ERROR, "Archipelago BizHawk: Could not increase input buffer size");

                            break;
                        }
                    }

                    dynamic_string_grow_outcome outcome = dynamic_string_push(&l_input_buffer, output_byte);
                    if (outcome != DYNAMIC_STRING_SUCCESS) {
                        DebugMessage(M64MSG_ERROR, "Archipelago BizHawk: Could not increase input buffer size during push");
                        break;
                    }

                    // If they sent another line, we can read that on
                    // the next tick.
                    if (output_byte == '\n') {
                        break;
                    }
                }

                if (l_input_buffer.len > 0) {
                    l_output_buffer.len = 0;

                    process_message_outcome outcome = process_message(
                        &l_output_buffer,
                        &l_input_buffer
                    );

                    if (outcome == PROCESS_MESSAGE_SUCCESS) {
                        if (SDLNet_TCP_Send(l_client, l_output_buffer.buffer, l_output_buffer.len) >= l_output_buffer.len) {
                            // Message was sent successfully! Check for a
                            // new message to arrive on the same connection,
                            // next tick.
                        } else {
                            DebugMessage(M64MSG_ERROR, "Archipelago BizHawk: SDLNet_TCP_Send: %s", SDLNet_GetError());

                            // Connection seems bad, drop the output message
                            // on the floor, and try to reconnect.
                            // TODO? Retain the output message and try
                            // sending it again? Or would that cause
                            // more issues than it solves?
                            SDLNet_TCP_Close(l_client);
                            l_state = ARCHIPELAGO_BIZHAWK_DISCONNECTED;
                        }
                    } else if (outcome == PROCESS_MESSAGE_NO_MEMORY) {
                        DebugMessage(M64MSG_ERROR, "Archipelago BizHawk: Could not allocate output buffer");
                    } else if (outcome == PROCESS_MESSAGE_BAD_INPUT) {
                        // TODO? Improve the error message here by
                        // making process_message return the location
                        // in the input buffer where things were
                        // unexpected. Messages might be very long and
                        // get truncated in the error message.
                        DebugMessage(M64MSG_ERROR, "Archipelago BizHawk: Bad message: %.*s", (int)l_input_buffer.len, l_input_buffer.buffer);
                    } else { // outcome == PROCESS_MESSAGE_UNKNOWN_INPUT
                        DebugMessage(M64MSG_ERROR, "Archipelago BizHawk: Unexpected message: %.*s", (int)l_input_buffer.len, l_input_buffer.buffer);
                    }

                    l_input_buffer.len = 0;
                } else {
                    DebugMessage(M64MSG_INFO, "Archipelago BizHawk: SDLNet_TCP_Recv returned: %d. Resetting connection.", last_recv_output);

                    // Connection seems bad, try to reconnect.
                    SDLNet_TCP_Close(l_client);
                    l_state = ARCHIPELAGO_BIZHAWK_DISCONNECTED;
                }
            } else {
                DebugMessage(M64MSG_ERROR, "Archipelago BizHawk: SDLNet_CheckSockets: %s", SDLNet_GetError());

                // Connection seems bad, try to reconnect.
                SDLNet_TCP_Close(l_client);
                l_state = ARCHIPELAGO_BIZHAWK_DISCONNECTED;
            }
            break;
        }
        default: {
            DebugMessage(M64MSG_ERROR, "Archipelago BizHawk: Unexpected state: %d", l_state);
        }
    }

    return interval;
}

m64p_error archipelago_bizhawk_start()
{
    if (SDL_InitSubSystem(SDL_INIT_TIMER) < 0)
    {
        DebugMessage(M64MSG_ERROR, "Archipelago BizHawk: Could not initialize SDL timer subsystem. %s", SDL_GetError());
        return M64ERR_SYSTEM_FAIL;
    }

    if (SDLNet_Init() < 0)
    {
        DebugMessage(M64MSG_ERROR, "Archipelago BizHawk: Could not initialize SDL Net library. %s", SDLNet_GetError());
        return M64ERR_SYSTEM_FAIL;
    }

    l_timer_id = SDL_AddTimer(
         TICK_INTERVAL_MS,
         archipelago_bizhawk_tick,
         NULL
    );
    if (l_timer_id == 0) {
        DebugMessage(M64MSG_ERROR, "Archipelago BizHawk: Could not initialize SDL Net library. %s", SDL_GetError());
        return M64ERR_SYSTEM_FAIL;
    }

    dynamic_string_grow_outcome outcome = dynamic_string_ensure(&l_input_buffer, 1024);
    switch (outcome) {
        case DYNAMIC_STRING_SUCCESS:
            break;
        default:
            return M64ERR_NO_MEMORY;
    }

    return M64ERR_SUCCESS;
}

void archipelago_bizhawk_stop() {
    dynamic_string_free(&l_output_buffer);
    dynamic_string_free(&l_input_buffer);

    SDL_RemoveTimer(l_timer_id);
    SDLNet_Quit();
    SDL_QuitSubSystem(SDL_INIT_TIMER);

    return;
}
