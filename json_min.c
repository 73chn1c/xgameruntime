/*
 * Minimal, hand-rolled JSON parser - see json_min.h for rationale/scope.
 */

#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "json_min.h"

struct parser
{
    const char *p;
    const char *end;
};

static void skip_ws( struct parser *ps )
{
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')) ps->p++;
}

static struct json_value *parse_value( struct parser *ps );

static struct json_value *alloc_value( enum json_type type )
{
    struct json_value *v = calloc( 1, sizeof(*v) );
    if (v) v->type = type;
    return v;
}

static BOOL append_char( char **buf, SIZE_T *len, SIZE_T *cap, char c )
{
    if (*len + 1 > *cap)
    {
        SIZE_T newCap = *cap ? *cap * 2 : 16;
        char *newBuf = realloc( *buf, newCap );
        if (!newBuf) return FALSE;
        *buf = newBuf;
        *cap = newCap;
    }
    (*buf)[(*len)++] = c;
    return TRUE;
}

static BOOL append_utf8( char **buf, SIZE_T *len, SIZE_T *cap, UINT32 cp )
{
    if (cp < 0x80) return append_char( buf, len, cap, (char)cp );
    if (cp < 0x800)
    {
        return append_char( buf, len, cap, (char)(0xC0 | (cp >> 6)) ) &&
               append_char( buf, len, cap, (char)(0x80 | (cp & 0x3F)) );
    }
    if (cp < 0x10000)
    {
        return append_char( buf, len, cap, (char)(0xE0 | (cp >> 12)) ) &&
               append_char( buf, len, cap, (char)(0x80 | ((cp >> 6) & 0x3F)) ) &&
               append_char( buf, len, cap, (char)(0x80 | (cp & 0x3F)) );
    }
    return append_char( buf, len, cap, (char)(0xF0 | (cp >> 18)) ) &&
           append_char( buf, len, cap, (char)(0x80 | ((cp >> 12) & 0x3F)) ) &&
           append_char( buf, len, cap, (char)(0x80 | ((cp >> 6) & 0x3F)) ) &&
           append_char( buf, len, cap, (char)(0x80 | (cp & 0x3F)) );
}

static int hex_digit( char c )
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parses a JSON string literal (ps->p pointing at the opening '"') into a
 * freshly-allocated, NUL-terminated UTF-8 buffer. Handles the standard
 * escapes including \uXXXX (with surrogate-pair combining for astral
 * codepoints) - real JSON responses from Xbox Live services can contain
 * these in gamertags/display names. */
static BOOL parse_string_raw( struct parser *ps, char **out, SIZE_T *outLen )
{
    char *buf = NULL;
    SIZE_T len = 0, cap = 0;

    if (ps->p >= ps->end || *ps->p != '"') return FALSE;
    ps->p++;

    while (ps->p < ps->end && *ps->p != '"')
    {
        char c = *ps->p++;
        if (c == '\\')
        {
            if (ps->p >= ps->end) goto fail;
            c = *ps->p++;
            switch (c)
            {
            case '"': if (!append_char( &buf, &len, &cap, '"' )) goto fail; break;
            case '\\': if (!append_char( &buf, &len, &cap, '\\' )) goto fail; break;
            case '/': if (!append_char( &buf, &len, &cap, '/' )) goto fail; break;
            case 'b': if (!append_char( &buf, &len, &cap, '\b' )) goto fail; break;
            case 'f': if (!append_char( &buf, &len, &cap, '\f' )) goto fail; break;
            case 'n': if (!append_char( &buf, &len, &cap, '\n' )) goto fail; break;
            case 'r': if (!append_char( &buf, &len, &cap, '\r' )) goto fail; break;
            case 't': if (!append_char( &buf, &len, &cap, '\t' )) goto fail; break;
            case 'u':
            {
                UINT32 cp;
                int i, d;
                if (ps->end - ps->p < 4) goto fail;
                cp = 0;
                for (i = 0; i < 4; i++)
                {
                    if ((d = hex_digit( ps->p[i] )) < 0) goto fail;
                    cp = (cp << 4) | (UINT32)d;
                }
                ps->p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF)
                {
                    UINT32 lo;
                    if (ps->end - ps->p < 6 || ps->p[0] != '\\' || ps->p[1] != 'u') goto fail;
                    ps->p += 2;
                    lo = 0;
                    for (i = 0; i < 4; i++)
                    {
                        if ((d = hex_digit( ps->p[i] )) < 0) goto fail;
                        lo = (lo << 4) | (UINT32)d;
                    }
                    ps->p += 4;
                    if (lo < 0xDC00 || lo > 0xDFFF) goto fail;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                if (!append_utf8( &buf, &len, &cap, cp )) goto fail;
                break;
            }
            default: goto fail;
            }
        }
        else
        {
            if (!append_char( &buf, &len, &cap, c )) goto fail;
        }
    }
    if (ps->p >= ps->end || *ps->p != '"') goto fail;
    ps->p++;

    if (!append_char( &buf, &len, &cap, '\0' )) goto fail;
    *out = buf;
    *outLen = len - 1;
    return TRUE;

fail:
    free( buf );
    return FALSE;
}

static struct json_value *parse_object( struct parser *ps )
{
    struct json_value *v = alloc_value( JSON_OBJECT );
    UINT32 cap = 0;
    if (!v) return NULL;

    ps->p++; /* '{' */
    skip_ws( ps );
    if (ps->p < ps->end && *ps->p == '}') { ps->p++; return v; }

    for (;;)
    {
        char *key;
        SIZE_T keyLen;
        struct json_value *val;

        skip_ws( ps );
        if (!parse_string_raw( ps, &key, &keyLen )) goto fail;
        skip_ws( ps );
        if (ps->p >= ps->end || *ps->p != ':') { free( key ); goto fail; }
        ps->p++;
        skip_ws( ps );
        if (!(val = parse_value( ps ))) { free( key ); goto fail; }

        if (v->u.object.count + 1 > cap)
        {
            UINT32 newCap = cap ? cap * 2 : 4;
            char **newKeys = realloc( v->u.object.keys, newCap * sizeof(*newKeys) );
            struct json_value **newVals = realloc( v->u.object.values, newCap * sizeof(*newVals) );
            if (!newKeys || !newVals) { free( key ); json_free( val ); goto fail; }
            v->u.object.keys = newKeys;
            v->u.object.values = newVals;
            cap = newCap;
        }
        v->u.object.keys[v->u.object.count] = key;
        v->u.object.values[v->u.object.count] = val;
        v->u.object.count++;

        skip_ws( ps );
        if (ps->p >= ps->end) goto fail;
        if (*ps->p == ',') { ps->p++; continue; }
        if (*ps->p == '}') { ps->p++; break; }
        goto fail;
    }
    return v;

fail:
    json_free( v );
    return NULL;
}

static struct json_value *parse_array( struct parser *ps )
{
    struct json_value *v = alloc_value( JSON_ARRAY );
    UINT32 cap = 0;
    if (!v) return NULL;

    ps->p++; /* '[' */
    skip_ws( ps );
    if (ps->p < ps->end && *ps->p == ']') { ps->p++; return v; }

    for (;;)
    {
        struct json_value *item;

        skip_ws( ps );
        if (!(item = parse_value( ps ))) goto fail;

        if (v->u.array.count + 1 > cap)
        {
            UINT32 newCap = cap ? cap * 2 : 4;
            struct json_value **newItems = realloc( v->u.array.items, newCap * sizeof(*newItems) );
            if (!newItems) { json_free( item ); goto fail; }
            v->u.array.items = newItems;
            cap = newCap;
        }
        v->u.array.items[v->u.array.count++] = item;

        skip_ws( ps );
        if (ps->p >= ps->end) goto fail;
        if (*ps->p == ',') { ps->p++; continue; }
        if (*ps->p == ']') { ps->p++; break; }
        goto fail;
    }
    return v;

fail:
    json_free( v );
    return NULL;
}

static struct json_value *parse_value( struct parser *ps )
{
    skip_ws( ps );
    if (ps->p >= ps->end) return NULL;

    if (*ps->p == '{') return parse_object( ps );
    if (*ps->p == '[') return parse_array( ps );
    if (*ps->p == '"')
    {
        struct json_value *v = alloc_value( JSON_STRING );
        if (!v) return NULL;
        if (!parse_string_raw( ps, &v->u.string.data, &v->u.string.len )) { free( v ); return NULL; }
        return v;
    }
    if (!strncmp( ps->p, "true", 4 ) && (ps->end - ps->p >= 4))
    {
        struct json_value *v = alloc_value( JSON_BOOL );
        if (!v) return NULL;
        v->u.boolean = TRUE;
        ps->p += 4;
        return v;
    }
    if (!strncmp( ps->p, "false", 5 ) && (ps->end - ps->p >= 5))
    {
        struct json_value *v = alloc_value( JSON_BOOL );
        if (!v) return NULL;
        v->u.boolean = FALSE;
        ps->p += 5;
        return v;
    }
    if (!strncmp( ps->p, "null", 4 ) && (ps->end - ps->p >= 4))
    {
        ps->p += 4;
        return alloc_value( JSON_NULL );
    }
    if (*ps->p == '-' || (*ps->p >= '0' && *ps->p <= '9'))
    {
        char numBuf[64];
        SIZE_T numLen = 0;
        struct json_value *v;
        const char *start = ps->p;

        if (*ps->p == '-') ps->p++;
        while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') ps->p++;
        if (ps->p < ps->end && *ps->p == '.')
        {
            ps->p++;
            while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') ps->p++;
        }
        if (ps->p < ps->end && (*ps->p == 'e' || *ps->p == 'E'))
        {
            ps->p++;
            if (ps->p < ps->end && (*ps->p == '+' || *ps->p == '-')) ps->p++;
            while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') ps->p++;
        }
        numLen = (SIZE_T)(ps->p - start);
        if (!numLen || numLen >= sizeof(numBuf)) return NULL;
        memcpy( numBuf, start, numLen );
        numBuf[numLen] = '\0';

        v = alloc_value( JSON_NUMBER );
        if (!v) return NULL;
        v->u.number = strtod( numBuf, NULL );
        return v;
    }
    return NULL;
}

struct json_value *json_parse( const char *json, SIZE_T jsonLen )
{
    struct parser ps = { json, json + jsonLen };
    struct json_value *v;

    v = parse_value( &ps );
    if (!v) return NULL;
    skip_ws( &ps );
    /* Trailing garbage after the top-level value is tolerated (real Xbox
     * Live responses have sometimes carried a trailing newline in the
     * wild) - only a genuinely malformed value fails to parse at all. */
    return v;
}

void json_free( struct json_value *value )
{
    UINT32 i;

    if (!value) return;
    switch (value->type)
    {
    case JSON_STRING:
        free( value->u.string.data );
        break;
    case JSON_ARRAY:
        for (i = 0; i < value->u.array.count; i++) json_free( value->u.array.items[i] );
        free( value->u.array.items );
        break;
    case JSON_OBJECT:
        for (i = 0; i < value->u.object.count; i++)
        {
            free( value->u.object.keys[i] );
            json_free( value->u.object.values[i] );
        }
        free( value->u.object.keys );
        free( value->u.object.values );
        break;
    default:
        break;
    }
    free( value );
}

const struct json_value *json_get( const struct json_value *object, const char *key )
{
    UINT32 i;

    if (!object || object->type != JSON_OBJECT) return NULL;
    for (i = 0; i < object->u.object.count; i++)
        if (!strcmp( object->u.object.keys[i], key )) return object->u.object.values[i];
    return NULL;
}

BOOL json_get_string( const struct json_value *object, const char *key, char **out, SIZE_T *outLen )
{
    const struct json_value *v = json_get( object, key );
    if (!v || v->type != JSON_STRING) return FALSE;
    *out = v->u.string.data;
    if (outLen) *outLen = v->u.string.len;
    return TRUE;
}

BOOL json_get_object( const struct json_value *object, const char *key, const struct json_value **out )
{
    const struct json_value *v = json_get( object, key );
    if (!v || v->type != JSON_OBJECT) return FALSE;
    *out = v;
    return TRUE;
}

BOOL json_get_array( const struct json_value *object, const char *key, const struct json_value **out )
{
    const struct json_value *v = json_get( object, key );
    if (!v || v->type != JSON_ARRAY) return FALSE;
    *out = v;
    return TRUE;
}

BOOL json_get_number( const struct json_value *object, const char *key, double *out )
{
    const struct json_value *v = json_get( object, key );
    if (!v || v->type != JSON_NUMBER) return FALSE;
    *out = v->u.number;
    return TRUE;
}

UINT32 json_array_size( const struct json_value *array )
{
    if (!array || array->type != JSON_ARRAY) return 0;
    return array->u.array.count;
}

const struct json_value *json_array_at( const struct json_value *array, UINT32 index )
{
    if (!array || array->type != JSON_ARRAY || index >= array->u.array.count) return NULL;
    return array->u.array.items[index];
}
