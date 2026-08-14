/*
 * Minimal XML helpers for exactly the two flat, single-level message
 * shapes the "shim" (xodus-service) IPC protocol uses:
 *   <MSATokenRequest><ClientId>..</ClientId><AllowUi>..</AllowUi><MsaFullTrust>..</MsaFullTrust></MSATokenRequest>
 *   <MSATokenResponse><Token>..</Token><DeviceRps>..</DeviceRps>...</MSATokenResponse>
 * No nesting, no attributes, no namespaces - a real XML library (like
 * libxml2, which isn't packaged for this mingw toolchain) would be
 * substantial overkill and a needless new dependency for this.
 */
#ifndef __WINE_XGAMERUNTIME_SHIM_XML_H
#define __WINE_XGAMERUNTIME_SHIM_XML_H

/* Builds "<?xml version=\"1.0\"?><MSATokenRequest><ClientId>ESCAPED</ClientId><AllowUi>true|false</AllowUi><MsaFullTrust>true|false</MsaFullTrust></MSATokenRequest>".
 * XML-escapes clientId (only the 5 predefined entities are relevant for a
 * GUID-shaped app id, but escaped generically for safety). Caller frees
 * *out with free(). Returns FALSE on allocation failure. */
BOOL build_msa_token_request_xml( const char *clientId, BOOL allowUi, BOOL fullTrust, char **out, UINT16 *outLen );

/* Extracts the text content of a single top-level child tag (e.g. "Token")
 * from a flat XML document. Returns a freshly null-terminated copy the
 * caller must free(), or FALSE if the tag isn't present. Does not decode
 * entities beyond the 5 predefined ones (&amp; &lt; &gt; &quot; &apos;) -
 * sufficient for what these responses actually contain (opaque base64/JWT
 * tokens, which never legitimately contain literal '<' or unescaped '&'). */
BOOL xml_get_tag_text( const char *xml, SIZE_T xmlLen, const char *tag, char **out );

#endif
