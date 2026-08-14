/*
 * Minimal XML helpers - see shim_xml.h for rationale/scope.
 */

#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "shim_xml.h"

static BOOL append_str( char **buf, SIZE_T *len, SIZE_T *cap, const char *s, SIZE_T n )
{
    if (*len + n > *cap)
    {
        SIZE_T newCap = *cap ? *cap * 2 : 64;
        char *newBuf;
        while (newCap < *len + n) newCap *= 2;
        if (!(newBuf = realloc( *buf, newCap ))) return FALSE;
        *buf = newBuf;
        *cap = newCap;
    }
    memcpy( *buf + *len, s, n );
    *len += n;
    return TRUE;
}

static BOOL append_escaped( char **buf, SIZE_T *len, SIZE_T *cap, const char *s )
{
    for (; *s; s++)
    {
        const char *ent = NULL;
        switch (*s)
        {
        case '&': ent = "&amp;"; break;
        case '<': ent = "&lt;"; break;
        case '>': ent = "&gt;"; break;
        case '"': ent = "&quot;"; break;
        case '\'': ent = "&apos;"; break;
        default: if (!append_str( buf, len, cap, s, 1 )) return FALSE; continue;
        }
        if (!append_str( buf, len, cap, ent, strlen( ent ) )) return FALSE;
    }
    return TRUE;
}

/* sizeof(lit)-1 is computed by the compiler (not hand-counted, which was
 * the source of a real off-by-two bug caught by -Werror=array-bounds
 * during development), so it can never drift out of sync with the
 * literal it measures. */
#define APPEND_LIT(s) append_str( &buf, &len, &cap, (s), sizeof(s) - 1 )

BOOL build_msa_token_request_xml( const char *clientId, BOOL allowUi, BOOL fullTrust, char **out, UINT16 *outLen )
{
    char *buf = NULL;
    SIZE_T len = 0, cap = 0;

    if (!APPEND_LIT( "<?xml version=\"1.0\"?><MSATokenRequest><ClientId>" )) goto fail;
    if (clientId && !append_escaped( &buf, &len, &cap, clientId )) goto fail;
    if (!APPEND_LIT( "</ClientId><AllowUi>" )) goto fail;
    if (!(allowUi ? APPEND_LIT( "true" ) : APPEND_LIT( "false" ))) goto fail;
    if (!APPEND_LIT( "</AllowUi><MsaFullTrust>" )) goto fail;
    if (!(fullTrust ? APPEND_LIT( "true" ) : APPEND_LIT( "false" ))) goto fail;
    if (!APPEND_LIT( "</MsaFullTrust></MSATokenRequest>" )) goto fail;

    if (len > 0xffff)
    {
        /* the wire format's length field is a UINT16, same limit the
         * upstream xuser-ipc branch's build_msa_token_request enforces */
        free( buf );
        return FALSE;
    }

    *out = buf;
    *outLen = (UINT16)len;
    return TRUE;

fail:
    free( buf );
    return FALSE;
}

#undef APPEND_LIT

static BOOL unescape_into( const char *s, SIZE_T n, char **out )
{
    char *buf = malloc( n + 1 );
    SIZE_T i = 0, o = 0;
    if (!buf) return FALSE;

    while (i < n)
    {
        if (s[i] == '&')
        {
            if (i + 5 <= n && !memcmp( s + i, "&amp;", 5 )) { buf[o++] = '&'; i += 5; continue; }
            if (i + 4 <= n && !memcmp( s + i, "&lt;", 4 ) ) { buf[o++] = '<'; i += 4; continue; }
            if (i + 4 <= n && !memcmp( s + i, "&gt;", 4 ) ) { buf[o++] = '>'; i += 4; continue; }
            if (i + 6 <= n && !memcmp( s + i, "&quot;", 6 )) { buf[o++] = '"'; i += 6; continue; }
            if (i + 6 <= n && !memcmp( s + i, "&apos;", 6 )) { buf[o++] = '\''; i += 6; continue; }
        }
        buf[o++] = s[i++];
    }
    buf[o] = '\0';
    *out = buf;
    return TRUE;
}

BOOL xml_get_tag_text( const char *xml, SIZE_T xmlLen, const char *tag, char **out )
{
    char openTag[128], closeTag[128];
    int openLen, closeLen;
    const char *p, *end = xml + xmlLen;
    const char *contentStart, *contentEnd;


    openLen = snprintf( openTag, sizeof(openTag), "<%s>", tag );
    closeLen = snprintf( closeTag, sizeof(closeTag), "</%s>", tag );
    if (openLen <= 0 || closeLen <= 0 || (SIZE_T)openLen >= sizeof(openTag) || (SIZE_T)closeLen >= sizeof(closeTag))
        return FALSE;

    /* Find the FIRST occurrence of the exact open tag "<Tag>" (not
     * "<TagSomethingElse ...>") by requiring the byte right after the
     * match to start the close-tag search - since these messages are
     * always flat with no nested same-named tags, a simple substring scan
     * anchored on the full "<Tag>" (including the closing '>') is
     * unambiguous and doesn't need real tokenization. */
    for (p = xml; p + openLen <= end; p++)
    {
        if (!memcmp( p, openTag, (SIZE_T)openLen ))
        {
            contentStart = p + openLen;
            for (contentEnd = contentStart; contentEnd + closeLen <= end; contentEnd++)
            {
                if (!memcmp( contentEnd, closeTag, (SIZE_T)closeLen ))
                    return unescape_into( contentStart, (SIZE_T)(contentEnd - contentStart), out );
            }
            return FALSE; /* open tag found but never closed - malformed */
        }
    }
    return FALSE;
}
