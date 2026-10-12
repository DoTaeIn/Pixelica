/* Pixelica i18n: en/ko/ja switching, fallback, validation, transactional errors. */
#include <string.h>

#include "pixelica/i18n.h"
#include "test_util.h"

#define KO_START "\xEC\x8B\x9C" "\xEC\x9E\x91" /* U+C2DC U+C791 */
#define KO_QUIT  "\xEC\xA2\x85" "\xEB\xA3\x8C" /* U+C885 U+B8CC */
#define JA_START "\xE9\x96\x8B" "\xE5\xA7\x8B" /* U+958B U+59CB */

static const PIXELICA_I18N_Entry EN[] = {
    {"menu.start", "Start"}, {"menu.quit", "Quit"}, {"menu.empty", ""}};
static const PIXELICA_I18N_Entry KO[] = {
    {"menu.start", KO_START}, {"menu.quit", KO_QUIT}};
static const PIXELICA_I18N_Entry JA[] = {{"menu.start", JA_START}};

static const PIXELICA_I18N_Locale LOCALES[] = {
    {"en", EN, 3}, {"ko", KO, 2}, {"ja", JA, 1}};

static void test_switch_and_fallback(void)
{
    PIXELICA_I18N_Catalog cat;
    size_t bytes = 99;

    CHECK_EQ_LONG(pixelica_i18n_init(&cat, LOCALES, 3, "en"), PIXELICA_OK);
    CHECK(strcmp(pixelica_i18n_locale(&cat), "en") == 0);
    CHECK(strcmp(pixelica_i18n_get(&cat, "menu.start"), "Start") == 0);

    CHECK_EQ_LONG(pixelica_i18n_set_locale(&cat, "ko"), PIXELICA_OK);
    CHECK(strcmp(pixelica_i18n_get(&cat, "menu.start"), KO_START) == 0);
    CHECK(pixelica_i18n_get_n(&cat, "menu.quit", &bytes) == KO[1].text);
    CHECK_EQ_LONG(bytes, 6);

    CHECK_EQ_LONG(pixelica_i18n_set_locale(&cat, "ja"), PIXELICA_OK);
    CHECK(strcmp(pixelica_i18n_get(&cat, "menu.start"), JA_START) == 0);
    /* ja lacks menu.quit: falls back to the default locale (en). */
    CHECK(pixelica_i18n_get(&cat, "menu.quit") == EN[1].text);
    /* Empty text is a valid translation, not a miss. */
    CHECK_EQ_LONG(pixelica_i18n_set_locale(&cat, "ko"), PIXELICA_OK);
    bytes = 99;
    CHECK(pixelica_i18n_get_n(&cat, "menu.empty", &bytes) == EN[2].text);
    CHECK_EQ_LONG(bytes, 0);

    /* Starting in another default locale makes that the fallback. */
    CHECK_EQ_LONG(pixelica_i18n_init(&cat, LOCALES, 3, "ko"), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_i18n_set_locale(&cat, "ja"), PIXELICA_OK);
    CHECK(strcmp(pixelica_i18n_get(&cat, "menu.quit"), KO_QUIT) == 0);
}

static void test_missing(void)
{
    PIXELICA_I18N_Catalog cat;
    PIXELICA_I18N_Catalog zero;
    char key[] = "no.such.key";
    size_t bytes = 99;

    memset(&zero, 0, sizeof zero);
    CHECK_EQ_LONG(pixelica_i18n_init(&cat, LOCALES, 3, "en"), PIXELICA_OK);
    CHECK(pixelica_i18n_get(&cat, key) == key);
    CHECK(pixelica_i18n_get_n(&cat, key, &bytes) == key);
    CHECK_EQ_LONG(bytes, 11);
    CHECK(pixelica_i18n_get_n(&cat, NULL, &bytes)[0] == '\0');
    CHECK_EQ_LONG(bytes, 0);
    CHECK(pixelica_i18n_get(&cat, NULL)[0] == '\0');

    /* NULL / uninitialized catalogs degrade to the key. */
    CHECK(pixelica_i18n_get(NULL, key) == key);
    CHECK(pixelica_i18n_get(&zero, key) == key);
    CHECK(pixelica_i18n_locale(NULL) == NULL);
    CHECK(pixelica_i18n_locale(&zero) == NULL);
    CHECK_EQ_LONG(pixelica_i18n_set_locale(&zero, "en"), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_i18n_set_locale(NULL, "en"), PIXELICA_ERR_INVALID_ARG);
}

static void test_set_locale_errors(void)
{
    PIXELICA_I18N_Catalog cat;
    const char *long_name = "abcdefghijklmnopqrstuvwxyzabcdefg"; /* 33 */

    CHECK_EQ_LONG(pixelica_i18n_init(&cat, LOCALES, 3, "en"), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_i18n_set_locale(&cat, "ko"), PIXELICA_OK);

    CHECK_EQ_LONG(pixelica_i18n_set_locale(&cat, "fr"), PIXELICA_ERR_NOT_FOUND);
    CHECK_EQ_LONG(pixelica_i18n_set_locale(&cat, "EN"), PIXELICA_ERR_NOT_FOUND);
    CHECK_EQ_LONG(pixelica_i18n_set_locale(&cat, long_name), PIXELICA_ERR_NOT_FOUND);
    CHECK_EQ_LONG(pixelica_i18n_set_locale(&cat, NULL), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_i18n_set_locale(&cat, ""), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_i18n_set_locale(&cat, "k o"), PIXELICA_ERR_INVALID_ARG);
    CHECK(strcmp(pixelica_i18n_locale(&cat), "ko") == 0);
    CHECK(strcmp(pixelica_i18n_get(&cat, "menu.start"), KO_START) == 0);
}

static PIXELICA_Status init_one(const char *key, const char *text)
{
    PIXELICA_I18N_Entry e;
    PIXELICA_I18N_Locale l;
    PIXELICA_I18N_Catalog cat;

    e.key = key;
    e.text = text;
    l.name = "en";
    l.entries = &e;
    l.entry_count = 1;
    return pixelica_i18n_init(&cat, &l, 1, "en");
}

static void test_invalid_utf8(void)
{
    static const char *const bad[] = {
        "\x80",             /* stray continuation */
        "\xC0\x80",         /* overlong NUL */
        "\xC1\xBF",         /* overlong */
        "\xE0\x80\x80",     /* overlong 3-byte */
        "\xED\xA0\x80",     /* surrogate */
        "\xF0\x80\x80\x80", /* overlong 4-byte */
        "\xF4\x90\x80\x80", /* > U+10FFFF */
        "\xF5\x80\x80\x80", /* invalid lead */
        "\xFF",             /* invalid byte */
        "\xEC\x8B",         /* truncated */
        "ok\xEC",           /* truncated at end */
    };
    size_t i;

    CHECK_EQ_LONG(init_one("k", "ok " KO_START), PIXELICA_OK);
    CHECK_EQ_LONG(init_one("k", "\xF0\x9F\x98\x80"), PIXELICA_OK); /* U+1F600 */
    for (i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        CHECK_EQ_LONG(init_one("k", bad[i]), PIXELICA_ERR_INVALID_ARG);
        CHECK_EQ_LONG(init_one(bad[i], "v"), PIXELICA_ERR_INVALID_ARG);
    }
}

static void test_validation(void)
{
    static char big[PIXELICA_I18N_MAX_TEXT_BYTES + 2];
    static PIXELICA_I18N_Locale many[PIXELICA_I18N_MAX_LOCALES + 1];
    static const PIXELICA_I18N_Entry dup[] = {{"a", "1"}, {"b", "2"}, {"a", "3"}};
    static const PIXELICA_I18N_Entry null_key[] = {{NULL, "1"}};
    static const PIXELICA_I18N_Entry null_text[] = {{"a", NULL}};
    static const PIXELICA_I18N_Entry one[] = {{"a", "1"}};
    PIXELICA_I18N_Catalog cat;
    PIXELICA_I18N_Locale l[2];
    size_t i;

    /* NULL arguments and zero counts. */
    CHECK_EQ_LONG(pixelica_i18n_init(NULL, LOCALES, 3, "en"), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_i18n_init(&cat, NULL, 3, "en"), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_i18n_init(&cat, LOCALES, 0, "en"), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_i18n_init(&cat, LOCALES, 3, NULL), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_i18n_init(&cat, LOCALES, 3, "fr"), PIXELICA_ERR_NOT_FOUND);

    /* Entry-level nulls and duplicates. */
    l[0].name = "en";
    l[0].entries = dup;
    l[0].entry_count = 3;
    CHECK_EQ_LONG(pixelica_i18n_init(&cat, l, 1, "en"), PIXELICA_ERR_INVALID_ARG);
    l[0].entries = null_key;
    l[0].entry_count = 1;
    CHECK_EQ_LONG(pixelica_i18n_init(&cat, l, 1, "en"), PIXELICA_ERR_INVALID_ARG);
    l[0].entries = null_text;
    CHECK_EQ_LONG(pixelica_i18n_init(&cat, l, 1, "en"), PIXELICA_ERR_INVALID_ARG);
    l[0].entries = NULL; /* count > 0 with no array */
    CHECK_EQ_LONG(pixelica_i18n_init(&cat, l, 1, "en"), PIXELICA_ERR_INVALID_ARG);
    l[0].entry_count = 0; /* empty locale without array is fine */
    CHECK_EQ_LONG(pixelica_i18n_init(&cat, l, 1, "en"), PIXELICA_OK);
    CHECK(strcmp(pixelica_i18n_get(&cat, "a"), "a") == 0);

    /* Duplicate and malformed locale names. */
    l[0].name = "en"; l[0].entries = one; l[0].entry_count = 1;
    l[1] = l[0];
    CHECK_EQ_LONG(pixelica_i18n_init(&cat, l, 2, "en"), PIXELICA_ERR_INVALID_ARG);
    l[1].name = "En"; /* names are case-sensitive, so distinct */
    CHECK_EQ_LONG(pixelica_i18n_init(&cat, l, 2, "en"), PIXELICA_OK);
    l[1].name = NULL;
    CHECK_EQ_LONG(pixelica_i18n_init(&cat, l, 2, "en"), PIXELICA_ERR_INVALID_ARG);
    l[1].name = "";
    CHECK_EQ_LONG(pixelica_i18n_init(&cat, l, 2, "en"), PIXELICA_ERR_INVALID_ARG);
    l[1].name = "e n";
    CHECK_EQ_LONG(pixelica_i18n_init(&cat, l, 2, "en"), PIXELICA_ERR_INVALID_ARG);
    l[1].name = "ko" KO_START;
    CHECK_EQ_LONG(pixelica_i18n_init(&cat, l, 2, "en"), PIXELICA_ERR_INVALID_ARG);

    /* Bounds. */
    l[0].name = "abcdefghijklmnopqrstuvwxyzabcdefg"; /* 33 */
    CHECK_EQ_LONG(pixelica_i18n_init(&cat, l, 1, "en"), PIXELICA_ERR_CAPACITY);
    l[0].name = "abcdefghijklmnopqrstuvwxyzabcdef"; /* 32 */
    CHECK_EQ_LONG(pixelica_i18n_init(&cat, l, 1, l[0].name), PIXELICA_OK);
    l[0].name = "en";
    l[0].entry_count = PIXELICA_I18N_MAX_ENTRIES + 1;
    CHECK_EQ_LONG(pixelica_i18n_init(&cat, l, 1, "en"), PIXELICA_ERR_CAPACITY);

    for (i = 0; i < PIXELICA_I18N_MAX_LOCALES + 1; ++i) {
        static const char *const names[] = {"a", "b", "c", "d", "e", "f", "g", "h",
                                            "i", "j", "k", "l", "m", "n", "o", "p", "q"};
        many[i].name = names[i];
        many[i].entries = NULL;
        many[i].entry_count = 0;
    }
    CHECK_EQ_LONG(pixelica_i18n_init(&cat, many, PIXELICA_I18N_MAX_LOCALES, "a"), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_i18n_init(&cat, many, PIXELICA_I18N_MAX_LOCALES + 1, "a"),
                  PIXELICA_ERR_CAPACITY);

    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    big[PIXELICA_I18N_MAX_TEXT_BYTES] = '\0'; /* exactly the maximum */
    CHECK_EQ_LONG(init_one("k", big), PIXELICA_OK);
    big[PIXELICA_I18N_MAX_TEXT_BYTES] = 'x'; /* one over */
    CHECK_EQ_LONG(init_one("k", big), PIXELICA_ERR_CAPACITY);
    big[PIXELICA_I18N_MAX_KEY_BYTES] = '\0';
    CHECK_EQ_LONG(init_one(big, "v"), PIXELICA_OK);
    big[PIXELICA_I18N_MAX_KEY_BYTES] = 'x';
    big[PIXELICA_I18N_MAX_KEY_BYTES + 1] = '\0';
    CHECK_EQ_LONG(init_one(big, "v"), PIXELICA_ERR_CAPACITY);
    CHECK_EQ_LONG(init_one("", "v"), PIXELICA_ERR_INVALID_ARG);
}

static void test_transactional_reinit(void)
{
    static const PIXELICA_I18N_Entry bad_entries[] = {{"menu.start", "\xFF"}};
    static const PIXELICA_I18N_Entry fr_entries[] = {{"menu.start", "Commencer"}};
    static const PIXELICA_I18N_Locale bad[] = {{"fr", bad_entries, 1}};
    static const PIXELICA_I18N_Locale fr[] = {{"fr", fr_entries, 1}};
    PIXELICA_I18N_Catalog cat;

    CHECK_EQ_LONG(pixelica_i18n_init(&cat, LOCALES, 3, "en"), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_i18n_set_locale(&cat, "ja"), PIXELICA_OK);

    /* A failed re-init keeps the previous catalog and selection. */
    CHECK_EQ_LONG(pixelica_i18n_init(&cat, bad, 1, "fr"), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_i18n_init(&cat, LOCALES, 3, "xx"), PIXELICA_ERR_NOT_FOUND);
    CHECK(strcmp(pixelica_i18n_locale(&cat), "ja") == 0);
    CHECK(strcmp(pixelica_i18n_get(&cat, "menu.start"), JA_START) == 0);
    CHECK(pixelica_i18n_get(&cat, "menu.quit") == EN[1].text);

    /* A successful re-init replaces everything and resets the selection. */
    CHECK_EQ_LONG(pixelica_i18n_init(&cat, fr, 1, "fr"), PIXELICA_OK);
    CHECK(strcmp(pixelica_i18n_locale(&cat), "fr") == 0);
    CHECK(strcmp(pixelica_i18n_get(&cat, "menu.start"), "Commencer") == 0);
    CHECK_EQ_LONG(pixelica_i18n_set_locale(&cat, "en"), PIXELICA_ERR_NOT_FOUND);
}

int main(void)
{
    test_switch_and_fallback();
    test_missing();
    test_set_locale_errors();
    test_invalid_utf8();
    test_validation();
    test_transactional_reinit();
    return pixelica_t_finish("test_i18n");
}
