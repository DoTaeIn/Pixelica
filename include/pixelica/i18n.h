/*
 * Pixelica - minimal multilingual UTF-8 string catalogs.
 *
 * No heap, no I/O, no globals: the caller owns every table and string. A
 * catalog is a borrowed view over locale tables plus a current locale and a
 * fallback (default) locale. Not thread-safe. Out of scope: printf-style
 * formatting, plurals, shaping/RTL, gettext/.po parsing.
 *
 * Lifetime: pixelica_i18n_init() borrows `locales`, every entry array and every
 * string. They must stay alive and unmodified until the catalog is
 * re-initialized or abandoned. Strings returned by get are borrowed from those
 * tables (or are the caller's own key), so they are NOT invalidated by
 * set_locale, but UI widgets that cache them must be refreshed after a switch.
 *
 * Re-init: init may be called again on the same catalog at any time. It
 * validates everything first and writes *out only on success, so a failed init
 * leaves the previous catalog (and its selection) intact. A successful init
 * resets current and fallback to the given default locale.
 */
#ifndef PIXELICA_I18N_H
#define PIXELICA_I18N_H

#include <stddef.h>
#include <stdint.h>

#include "pixelica/status.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PIXELICA_I18N_MAX_LOCALES      16u
#define PIXELICA_I18N_MAX_ENTRIES      1024u /* per locale */
#define PIXELICA_I18N_MAX_LOCALE_BYTES 32u   /* excluding the NUL */
#define PIXELICA_I18N_MAX_KEY_BYTES    128u  /* excluding the NUL */
#define PIXELICA_I18N_MAX_TEXT_BYTES   4096u /* excluding the NUL; matches UI */

/* key/text: NUL-terminated, strict UTF-8 (no overlongs, surrogates, > U+10FFFF
 * or embedded NUL). key is 1..MAX_KEY_BYTES bytes, text is 0..MAX_TEXT_BYTES. */
typedef struct PIXELICA_I18N_Entry {
    const char *key;
    const char *text;
} PIXELICA_I18N_Entry;

/* name: 1..MAX_LOCALE_BYTES of [A-Za-z0-9_-], e.g. "en", "ko", "ja", "pt-BR".
 * Compared case-sensitively. entries may be NULL only when entry_count is 0.
 * Entries need not be sorted; keys must be unique within a locale. */
typedef struct PIXELICA_I18N_Locale {
    const char *name;
    const PIXELICA_I18N_Entry *entries;
    size_t entry_count;
} PIXELICA_I18N_Locale;

/* Treat as opaque; use the functions below. */
typedef struct PIXELICA_I18N_Catalog {
    const PIXELICA_I18N_Locale *locales;
    size_t locale_count;
    size_t current, fallback;
    uint32_t magic;
} PIXELICA_I18N_Catalog;

/* Validates all locales/entries, then selects default_locale as both current
 * and fallback. 1..MAX_LOCALES locales.
 *   INVALID_ARG: NULL, bad name/key/text, invalid UTF-8, duplicate locale or key
 *   CAPACITY:    too many locales/entries, or a name/key/text over its limit
 *   NOT_FOUND:   default_locale is not one of the locales
 * On any error *out is unchanged. */
PIXELICA_Status pixelica_i18n_init(PIXELICA_I18N_Catalog *out,
    const PIXELICA_I18N_Locale *locales, size_t locale_count,
    const char *default_locale);

/* Switches the current locale. INVALID_ARG for NULL/uninitialized catalog or a
 * malformed name; NOT_FOUND for an unknown locale. Errors leave the selection
 * unchanged. The fallback locale never changes. */
PIXELICA_Status pixelica_i18n_set_locale(PIXELICA_I18N_Catalog *cat, const char *name);

/* Current locale name, or NULL for a NULL/uninitialized catalog. Borrowed. */
const char *pixelica_i18n_locale(const PIXELICA_I18N_Catalog *cat);

/* Looks key up in the current locale, then in the fallback locale. A missing
 * key (or NULL/uninitialized catalog) returns the key itself; a NULL key
 * returns "". The key must be NUL-terminated. Never returns NULL.
 * get_n also stores the byte length (without NUL) in *out_bytes when out_bytes
 * is non-NULL, ready for PIXELICA_UI_Widget.text/text_bytes. */
const char *pixelica_i18n_get(const PIXELICA_I18N_Catalog *cat, const char *key);
const char *pixelica_i18n_get_n(const PIXELICA_I18N_Catalog *cat, const char *key,
    size_t *out_bytes);

#ifdef __cplusplus
}
#endif

#endif /* PIXELICA_I18N_H */
