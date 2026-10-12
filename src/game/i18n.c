/*
 * Pixelica - minimal multilingual UTF-8 string catalogs.
 * See include/pixelica/i18n.h for the contract. No heap, no globals.
 */
#include <string.h>

#include "pixelica/i18n.h"

#define PIXELICA_I18N_MAGIC 0x49313831u

/* Length of s if it is NUL-terminated within max bytes, else max + 1. */
static size_t bounded_len(const char *s, size_t max)
{
    size_t n = 0;

    while (n <= max && s[n] != '\0') {
        ++n;
    }
    return n;
}

static int is_cont(const unsigned char *s, size_t i, size_t n)
{
    return i < n && (s[i] & 0xC0u) == 0x80u;
}

/* Strict UTF-8 (RFC 3629): no overlongs, surrogates or > U+10FFFF. */
static int utf8_valid(const char *text, size_t n)
{
    const unsigned char *s = (const unsigned char *)text;
    size_t i = 0;

    while (i < n) {
        unsigned c = s[i];

        if (c < 0x80u) {
            i += 1;
        } else if (c >= 0xC2u && c <= 0xDFu) {
            if (!is_cont(s, i + 1, n)) {
                return 0;
            }
            i += 2;
        } else if (c >= 0xE0u && c <= 0xEFu) {
            if (!is_cont(s, i + 1, n) || !is_cont(s, i + 2, n)) {
                return 0;
            }
            if ((c == 0xE0u && s[i + 1] < 0xA0u) ||
                (c == 0xEDu && s[i + 1] > 0x9Fu)) {
                return 0;
            }
            i += 3;
        } else if (c >= 0xF0u && c <= 0xF4u) {
            if (!is_cont(s, i + 1, n) || !is_cont(s, i + 2, n) ||
                !is_cont(s, i + 3, n)) {
                return 0;
            }
            if ((c == 0xF0u && s[i + 1] < 0x90u) ||
                (c == 0xF4u && s[i + 1] > 0x8Fu)) {
                return 0;
            }
            i += 4;
        } else {
            return 0;
        }
    }
    return 1;
}

static PIXELICA_Status check_string(const char *s, size_t min, size_t max)
{
    size_t n;

    if (s == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    n = bounded_len(s, max);
    if (n > max) {
        return PIXELICA_ERR_CAPACITY;
    }
    if (n < min || !utf8_valid(s, n)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    return PIXELICA_OK;
}

static int name_char_ok(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_';
}

static PIXELICA_Status check_name(const char *name)
{
    size_t n, i;

    if (name == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    n = bounded_len(name, PIXELICA_I18N_MAX_LOCALE_BYTES);
    if (n > PIXELICA_I18N_MAX_LOCALE_BYTES) {
        return PIXELICA_ERR_CAPACITY;
    }
    if (n == 0) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    for (i = 0; i < n; ++i) {
        if (!name_char_ok(name[i])) {
            return PIXELICA_ERR_INVALID_ARG;
        }
    }
    return PIXELICA_OK;
}

static PIXELICA_Status check_locale(const PIXELICA_I18N_Locale *loc)
{
    PIXELICA_Status st = check_name(loc->name);
    size_t i, j;

    if (st != PIXELICA_OK) {
        return st;
    }
    if (loc->entry_count > PIXELICA_I18N_MAX_ENTRIES) {
        return PIXELICA_ERR_CAPACITY;
    }
    if (loc->entry_count > 0 && loc->entries == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    for (i = 0; i < loc->entry_count; ++i) {
        st = check_string(loc->entries[i].key, 1, PIXELICA_I18N_MAX_KEY_BYTES);
        if (st != PIXELICA_OK) {
            return st;
        }
        st = check_string(loc->entries[i].text, 0, PIXELICA_I18N_MAX_TEXT_BYTES);
        if (st != PIXELICA_OK) {
            return st;
        }
        for (j = 0; j < i; ++j) {
            if (strcmp(loc->entries[i].key, loc->entries[j].key) == 0) {
                return PIXELICA_ERR_INVALID_ARG;
            }
        }
    }
    return PIXELICA_OK;
}

/* Index of the locale called name, or count when absent. name is validated. */
static size_t find_locale(const PIXELICA_I18N_Locale *locales, size_t count,
                          const char *name)
{
    size_t i;

    for (i = 0; i < count; ++i) {
        if (strcmp(locales[i].name, name) == 0) {
            break;
        }
    }
    return i;
}

static int catalog_ok(const PIXELICA_I18N_Catalog *cat)
{
    return cat != NULL && cat->magic == PIXELICA_I18N_MAGIC &&
           cat->locales != NULL && cat->current < cat->locale_count &&
           cat->fallback < cat->locale_count;
}

PIXELICA_Status pixelica_i18n_init(PIXELICA_I18N_Catalog *out,
    const PIXELICA_I18N_Locale *locales, size_t locale_count,
    const char *default_locale)
{
    PIXELICA_Status st;
    size_t i, def;

    if (out == NULL || locales == NULL || locale_count == 0) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (locale_count > PIXELICA_I18N_MAX_LOCALES) {
        return PIXELICA_ERR_CAPACITY;
    }
    st = check_name(default_locale);
    if (st != PIXELICA_OK) {
        return st;
    }
    for (i = 0; i < locale_count; ++i) {
        st = check_locale(&locales[i]);
        if (st != PIXELICA_OK) {
            return st;
        }
        if (find_locale(locales, i, locales[i].name) != i) {
            return PIXELICA_ERR_INVALID_ARG;
        }
    }
    def = find_locale(locales, locale_count, default_locale);
    if (def == locale_count) {
        return PIXELICA_ERR_NOT_FOUND;
    }
    out->locales = locales;
    out->locale_count = locale_count;
    out->current = def;
    out->fallback = def;
    out->magic = PIXELICA_I18N_MAGIC;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_i18n_set_locale(PIXELICA_I18N_Catalog *cat, const char *name)
{
    PIXELICA_Status st;
    size_t idx;

    if (!catalog_ok(cat)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    st = check_name(name);
    if (st == PIXELICA_ERR_CAPACITY) {
        st = PIXELICA_ERR_NOT_FOUND; /* too long to be any valid locale */
    }
    if (st != PIXELICA_OK) {
        return st;
    }
    idx = find_locale(cat->locales, cat->locale_count, name);
    if (idx == cat->locale_count) {
        return PIXELICA_ERR_NOT_FOUND;
    }
    cat->current = idx;
    return PIXELICA_OK;
}

const char *pixelica_i18n_locale(const PIXELICA_I18N_Catalog *cat)
{
    return catalog_ok(cat) ? cat->locales[cat->current].name : NULL;
}

static const char *lookup(const PIXELICA_I18N_Locale *loc, const char *key)
{
    size_t i;

    for (i = 0; i < loc->entry_count; ++i) {
        if (strcmp(loc->entries[i].key, key) == 0) {
            return loc->entries[i].text;
        }
    }
    return NULL;
}

const char *pixelica_i18n_get_n(const PIXELICA_I18N_Catalog *cat, const char *key,
    size_t *out_bytes)
{
    const char *text = NULL;

    if (key == NULL) {
        if (out_bytes != NULL) {
            *out_bytes = 0;
        }
        return "";
    }
    if (catalog_ok(cat)) {
        text = lookup(&cat->locales[cat->current], key);
        if (text == NULL && cat->fallback != cat->current) {
            text = lookup(&cat->locales[cat->fallback], key);
        }
    }
    if (text == NULL) {
        text = key;
    }
    if (out_bytes != NULL) {
        *out_bytes = strlen(text);
    }
    return text;
}

const char *pixelica_i18n_get(const PIXELICA_I18N_Catalog *cat, const char *key)
{
    return pixelica_i18n_get_n(cat, key, NULL);
}
