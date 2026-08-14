/*
 * Minimal, hand-rolled JSON parser for the small set of read-only
 * operations this fork's real Xbox Live SISU-auth flow needs
 * (device_auth/sisu_auth response parsing) - written specifically to
 * avoid a dependency on a full Windows.Data.Json WinRT component (which
 * does not exist anywhere in this fork) or a mingw-cross-compiled
 * libxml2/JSON library (neither packaged for this toolchain either).
 *
 * Deliberately narrow scope: parses into a simple tagged-union tree,
 * supports objects/arrays/strings/numbers/bool/null - exactly what the
 * real Xbox Live SISU/device-auth JSON responses use, nothing more
 * (no comments, no trailing commas, strict RFC 8259 subset).
 */
#ifndef __WINE_XGAMERUNTIME_JSON_MIN_H
#define __WINE_XGAMERUNTIME_JSON_MIN_H

enum json_type
{
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT,
};

struct json_value
{
    enum json_type type;
    union
    {
        BOOL boolean;
        double number;
        struct { char *data; SIZE_T len; } string;
        struct { struct json_value **items; UINT32 count; } array;
        struct { char **keys; struct json_value **values; UINT32 count; } object;
    } u;
};

/* Parses `json` (jsonLen bytes, need not be NUL-terminated) into a tree.
 * Returns NULL on any malformed input. Caller must json_free() the result. */
struct json_value *json_parse( const char *json, SIZE_T jsonLen );
void json_free( struct json_value *value );

/* All getters return NULL/FALSE on a missing/wrong-type key - never
 * partially fill output, matching the WEB_E_JSON_VALUE_NOT_FOUND
 * "not found is a normal, checkable outcome" convention the ported
 * xuser.c code already expects. */
const struct json_value *json_get( const struct json_value *object, const char *key );
BOOL json_get_string( const struct json_value *object, const char *key, char **out, SIZE_T *outLen );
BOOL json_get_object( const struct json_value *object, const char *key, const struct json_value **out );
BOOL json_get_array( const struct json_value *object, const char *key, const struct json_value **out );
BOOL json_get_number( const struct json_value *object, const char *key, double *out );
UINT32 json_array_size( const struct json_value *array );
const struct json_value *json_array_at( const struct json_value *array, UINT32 index );

#endif
