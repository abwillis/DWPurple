/*
 * DWPurple - message display helpers: HTML -> text, ":code:" emoji and
 * reaction-notice detection (bridged networks send "Reacted with :x:").
 */
#include "dwpurple.h"

#include <stdio.h>
#include <glib/gstdio.h>

typedef struct {
    const char *code;
    const char *glyph;     /* UTF-8 */
} EmojiEntry;

static const EmojiEntry builtin[] = {
    { "smile", "\xF0\x9F\x99\x82" },
    { "slightly_smiling_face", "\xF0\x9F\x99\x82" },
    { "smiley", "\xF0\x9F\x98\x83" },
    { "grin", "\xF0\x9F\x98\x81" },
    { "grinning", "\xF0\x9F\x98\x80" },
    { "laugh", "\xF0\x9F\x98\x86" },
    { "laughing", "\xF0\x9F\x98\x86" },
    { "joy", "\xF0\x9F\x98\x82" },
    { "rofl", "\xF0\x9F\xA4\xA3" },
    { "wink", "\xF0\x9F\x98\x89" },
    { "blush", "\xF0\x9F\x98\x8A" },
    { "innocent", "\xF0\x9F\x98\x87" },
    { "heart_eyes", "\xF0\x9F\x98\x8D" },
    { "kiss", "\xF0\x9F\x98\x98" },
    { "kissing_heart", "\xF0\x9F\x98\x98" },
    { "yum", "\xF0\x9F\x98\x8B" },
    { "stuck_out_tongue", "\xF0\x9F\x98\x9B" },
    { "tongueout", "\xF0\x9F\x98\x9B" },
    { "sunglasses", "\xF0\x9F\x98\x8E" },
    { "cool", "\xF0\x9F\x98\x8E" },
    { "thinking", "\xF0\x9F\xA4\x94" },
    { "neutral_face", "\xF0\x9F\x98\x90" },
    { "expressionless", "\xF0\x9F\x98\x91" },
    { "unamused", "\xF0\x9F\x98\x92" },
    { "rolling_eyes", "\xF0\x9F\x99\x84" },
    { "smirk", "\xF0\x9F\x98\x8F" },
    { "sad", "\xF0\x9F\x99\x81" },
    { "slightly_frowning_face", "\xF0\x9F\x99\x81" },
    { "disappointed", "\xF0\x9F\x98\x9E" },
    { "worried", "\xF0\x9F\x98\x9F" },
    { "cry", "\xF0\x9F\x98\xA2" },
    { "sob", "\xF0\x9F\x98\xAD" },
    { "angry", "\xF0\x9F\x98\xA0" },
    { "rage", "\xF0\x9F\x98\xA1" },
    { "surprised", "\xF0\x9F\x98\xAE" },
    { "open_mouth", "\xF0\x9F\x98\xAE" },
    { "astonished", "\xF0\x9F\x98\xB2" },
    { "scream", "\xF0\x9F\x98\xB1" },
    { "flushed", "\xF0\x9F\x98\xB3" },
    { "sweat_smile", "\xF0\x9F\x98\x85" },
    { "sleeping", "\xF0\x9F\x98\xB4" },
    { "sleepy", "\xF0\x9F\x98\xAA" },
    { "mask", "\xF0\x9F\x98\xB7" },
    { "nerd_face", "\xF0\x9F\xA4\x93" },
    { "upside_down_face", "\xF0\x9F\x99\x83" },
    { "hugs", "\xF0\x9F\xA4\x97" },
    { "hug", "\xF0\x9F\xA4\x97" },
    { "facepalm", "\xF0\x9F\xA4\xA6" },
    { "shrug", "\xF0\x9F\xA4\xB7" },
    { "party", "\xF0\x9F\xA5\xB3" },
    { "partying_face", "\xF0\x9F\xA5\xB3" },
    { "zany_face", "\xF0\x9F\xA4\xAA" },
    { "heart", "\xE2\x9D\xA4\xEF\xB8\x8F" },
    { "broken_heart", "\xF0\x9F\x92\x94" },
    { "sparkling_heart", "\xF0\x9F\x92\x96" },
    { "two_hearts", "\xF0\x9F\x92\x95" },
    { "like", "\xF0\x9F\x91\x8D" },
    { "yes", "\xF0\x9F\x91\x8D" },
    { "+1", "\xF0\x9F\x91\x8D" },
    { "thumbsup", "\xF0\x9F\x91\x8D" },
    { "-1", "\xF0\x9F\x91\x8E" },
    { "thumbsdown", "\xF0\x9F\x91\x8E" },
    { "no", "\xF0\x9F\x91\x8E" },
    { "ok_hand", "\xF0\x9F\x91\x8C" },
    { "clap", "\xF0\x9F\x91\x8F" },
    { "wave", "\xF0\x9F\x91\x8B" },
    { "pray", "\xF0\x9F\x99\x8F" },
    { "muscle", "\xF0\x9F\x92\xAA" },
    { "raised_hands", "\xF0\x9F\x99\x8C" },
    { "point_up", "\xE2\x98\x9D\xEF\xB8\x8F" },
    { "v", "\xE2\x9C\x8C\xEF\xB8\x8F" },
    { "handshake", "\xF0\x9F\xA4\x9D" },
    { "eyes", "\xF0\x9F\x91\x80" },
    { "fire", "\xF0\x9F\x94\xA5" },
    { "100", "\xF0\x9F\x92\xAF" },
    { "tada", "\xF0\x9F\x8E\x89" },
    { "star", "\xE2\xAD\x90" },
    { "sparkles", "\xE2\x9C\xA8" },
    { "rainbow", "\xF0\x9F\x8C\x88" },
    { "sun", "\xE2\x98\x80\xEF\xB8\x8F" },
    { "sunny", "\xE2\x98\x80\xEF\xB8\x8F" },
    { "cloud", "\xE2\x98\x81\xEF\xB8\x8F" },
    { "zap", "\xE2\x9A\xA1" },
    { "snowflake", "\xE2\x9D\x84\xEF\xB8\x8F" },
    { "coffee", "\xE2\x98\x95" },
    { "beer", "\xF0\x9F\x8D\xBA" },
    { "beers", "\xF0\x9F\x8D\xBB" },
    { "pizza", "\xF0\x9F\x8D\x95" },
    { "cake", "\xF0\x9F\x8D\xB0" },
    { "gift", "\xF0\x9F\x8E\x81" },
    { "rocket", "\xF0\x9F\x9A\x80" },
    { "bulb", "\xF0\x9F\x92\xA1" },
    { "check", "\xE2\x9C\x94\xEF\xB8\x8F" },
    { "heavy_check_mark", "\xE2\x9C\x94\xEF\xB8\x8F" },
    { "white_check_mark", "\xE2\x9C\x85" },
    { "x", "\xE2\x9D\x8C" },
    { "warning", "\xE2\x9A\xA0\xEF\xB8\x8F" },
    { "question", "\xE2\x9D\x93" },
    { "exclamation", "\xE2\x9D\x97" },
    { "skull", "\xF0\x9F\x92\x80" },
    { "poop", "\xF0\x9F\x92\xA9" },
    { "ghost", "\xF0\x9F\x91\xBB" },
    { "robot", "\xF0\x9F\xA4\x96" },
    { "see_no_evil", "\xF0\x9F\x99\x88" },
    { "dog", "\xF0\x9F\x90\xB6" },
    { "cat", "\xF0\x9F\x90\xB1" },
};

static const char *const picker_codes[] = {
    "smile",
    "grin",
    "joy",
    "rofl",
    "wink",
    "blush",
    "heart_eyes",
    "kiss",
    "sunglasses",
    "thinking",
    "neutral_face",
    "rolling_eyes",
    "smirk",
    "sad",
    "cry",
    "sob",
    "angry",
    "surprised",
    "scream",
    "flushed",
    "sweat_smile",
    "sleeping",
    "upside_down_face",
    "hug",
    "facepalm",
    "shrug",
    "party",
    "heart",
    "broken_heart",
    "like",
    "thumbsdown",
    "ok_hand",
    "clap",
    "wave",
    "pray",
    "muscle",
    "raised_hands",
    "handshake",
    "eyes",
    "fire",
    "100",
    "tada",
    "star",
    "sparkles",
    "rainbow",
    "coffee",
    "beer",
    "pizza",
    "cake",
    "gift",
    "rocket",
    "bulb",
    "white_check_mark",
    "x",
    "warning",
    "question",
    NULL
};

static GHashTable *builtin_map = NULL;
static GHashTable *user_map = NULL;
static char *user_path = NULL;
static time_t user_mtime = 0;
static long user_size = -1;

void dwp_emoji_add_prefs(void)
{
    purple_prefs_add_bool(DWP_PREF_SHOW_REACTIONS, TRUE);
#ifdef __OS2__
    /* Most OS/2 code pages cannot show emoji. */
    purple_prefs_add_bool(DWP_PREF_EMOJI_CODES, FALSE);
#else
    purple_prefs_add_bool(DWP_PREF_EMOJI_CODES, TRUE);
#endif
}

char *dwp_emoji_user_map_path(void)
{
    return g_build_filename(purple_user_dir(), "emoji.txt", NULL);
}

static void ensure_builtin(void)
{
    size_t z;

    if(builtin_map)
        return;
    builtin_map = g_hash_table_new(g_str_hash, g_str_equal);
    for(z = 0; z < sizeof(builtin) / sizeof(builtin[0]); z++)
        g_hash_table_insert(builtin_map, (gpointer)builtin[z].code, (gpointer)builtin[z].glyph);
}

/* Reload <config dir>/emoji.txt ("code = glyph" lines) when it changes. */
static void refresh_user_map(void)
{
    char *path = dwp_emoji_user_map_path();
    GStatBuf st;
    gboolean exists = g_stat(path, &st) == 0;
    time_t mtime = exists ? st.st_mtime : 0;
    long size = exists ? (long)st.st_size : -1;
    char *contents = NULL;
    char **lines;
    int z;

    if(user_map && purple_strequal(path, user_path) && mtime == user_mtime && size == user_size)
    {
        g_free(path);
        return;
    }
    g_free(user_path);
    user_path = path;
    user_mtime = mtime;
    user_size = size;
    if(user_map)
        g_hash_table_destroy(user_map);
    user_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    if(!exists || !g_file_get_contents(path, &contents, NULL, NULL))
        return;
    lines = g_strsplit(contents, "\n", -1);
    for(z = 0; lines[z]; z++)
    {
        char *line = g_strstrip(lines[z]), *eq, *code, *value;
        size_t len;

        if(!*line || *line == '#' || !(eq = strchr(line, '=')) || eq == line)
            continue;
        *eq = '\0';
        code = g_strstrip(line);
        value = g_strstrip(eq + 1);
        len = strlen(code);
        if(len > 2 && code[0] == ':' && code[len - 1] == ':')
        {
            code[len - 1] = '\0';
            code++;
        }
        if(*code && *value && g_utf8_validate(value, -1, NULL))
            g_hash_table_replace(user_map, g_ascii_strdown(code, -1), g_strdup(value));
    }
    g_strfreev(lines);
    g_free(contents);
}

const char *dwp_emoji_lookup(const char *code)
{
    char *key;
    const char *ret;

    ensure_builtin();
    refresh_user_map();
    key = g_ascii_strdown(code, -1);
    ret = user_map ? g_hash_table_lookup(user_map, key) : NULL;
    if(!ret)
        ret = g_hash_table_lookup(builtin_map, key);
    g_free(key);
    return ret;
}

const char *const *dwp_emoji_picker_codes(void)
{
    return picker_codes;
}

/* Value of attribute name in an HTML tag, newly allocated or NULL. */
static char *tag_attribute(const char *tag, const char *name)
{
    char *pattern = g_strdup_printf("\\b%s\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)'|([^\\s>]+))", name);
    GRegex *re = g_regex_new(pattern, G_REGEX_CASELESS, 0, NULL);
    GMatchInfo *mi = NULL;
    char *ret = NULL;
    int z;

    g_free(pattern);
    if(!re)
        return NULL;
    if(g_regex_match(re, tag, 0, &mi))
    {
        for(z = 1; z <= 3 && !ret; z++)
        {
            int start = -1, end = -1;

            if(g_match_info_fetch_pos(mi, z, &start, &end) && start >= 0)
                ret = g_strndup(tag + start, end - start);
        }
    }
    g_match_info_free(mi);
    g_regex_unref(re);
    return ret;
}

/* <img> tags: libpurple stored images become "[image]", others (blocked
 * remote images, custom emoticons) show their alt/title text. */
static char *replace_images(const char *html)
{
    static GRegex *img_re = NULL;
    GMatchInfo *mi = NULL;
    GString *out = g_string_new(NULL);
    const char *last = html;

    if(!img_re)
        img_re = g_regex_new("<img\\b[^>]*>", G_REGEX_CASELESS, 0, NULL);
    if(g_regex_match(img_re, html, 0, &mi))
    {
        do
        {
            int start, end;
            char *tag, *text = NULL;

            g_match_info_fetch_pos(mi, 0, &start, &end);
            g_string_append_len(out, last, (html + start) - last);
            last = html + end;
            tag = g_strndup(html + start, end - start);
            if(!(text = tag_attribute(tag, "id")))
            {
                text = tag_attribute(tag, "alt");
                if(!text || !*g_strstrip(text))
                {
                    g_free(text);
                    text = tag_attribute(tag, "title");
                }
                if(text)
                {
                    char *p;

                    /* Attribute values are HTML text; drop anything tag-like. */
                    for(p = text; *p; p++)
                        if(*p == '<' || *p == '>')
                            *p = ' ';
                }
            }
            else
            {
                PurpleStoredImage *img = purple_imgstore_find_by_id(atoi(text));
                const char *fname = img ? purple_imgstore_get_filename(img) : NULL;

                g_free(text);
                text = fname ? g_markup_printf_escaped("[image: %s]", fname) : g_strdup("[image]");
            }
            g_string_append(out, text && *g_strstrip(text) ? text : "[image]");
            g_free(text);
            g_free(tag);
        } while(g_match_info_next(mi, NULL));
    }
    g_match_info_free(mi);
    g_string_append(out, last);
    return g_string_free(out, FALSE);
}

static char *convert_codes(const char *text)
{
    static GRegex *code_re = NULL;
    GString *out = g_string_new(NULL);
    const char *last = text;
    int from = 0, len = (int)strlen(text);

    if(!code_re)
        code_re = g_regex_new(":([A-Za-z0-9_+\\-]{1,48}):", 0, 0, NULL);
    while(from < len)
    {
        GMatchInfo *mi = NULL;
        int start, end, cstart, cend;
        char *code;
        const char *glyph;

        if(!g_regex_match_full(code_re, text, -1, from, 0, &mi, NULL))
        {
            g_match_info_free(mi);
            break;
        }
        g_match_info_fetch_pos(mi, 0, &start, &end);
        g_match_info_fetch_pos(mi, 1, &cstart, &cend);
        g_match_info_free(mi);
        code = g_strndup(text + cstart, cend - cstart);
        glyph = dwp_emoji_lookup(code);
        g_free(code);
        if(!glyph)
        {
            /* Unknown: the closing ':' may open the next code ("a :x:y:"). */
            from = end - 1;
            continue;
        }
        g_string_append_len(out, last, (text + start) - last);
        g_string_append(out, glyph);
        last = text + end;
        from = end;
    }
    g_string_append(out, last);
    return g_string_free(out, FALSE);
}

char *dwp_html_to_text(const char *html, gboolean emoji_codes)
{
    char *noimg = replace_images(html ? html : "");
    char *plain = purple_markup_strip_html(noimg);
    char *ret;

    g_free(noimg);
    if(!emoji_codes)
        return plain;
    ret = convert_codes(plain);
    g_free(plain);
    return ret;
}

gboolean dwp_is_reaction(const char *plain)
{
    static GRegex *direct = NULL, *named = NULL, *code = NULL;
    char *text;
    gboolean ret = FALSE;
    const char *p;

    if(!direct)
    {
        /* "Reacted with a :rainbowsmile:" / "Reacted to ..." */
        direct = g_regex_new("^\\s*reacted\\s+(?:with|to)\\b", G_REGEX_CASELESS, 0, NULL);
        /* "Andy Smith reacted to your message with :like:" */
        named = g_regex_new("^\\s*(?:\\S+\\s+){1,3}reacted\\s+(?:with|to)\\b", G_REGEX_CASELESS, 0, NULL);
        code = g_regex_new(":[A-Za-z0-9_+\\-]{1,48}:", 0, 0, NULL);
    }
    if(!plain || !g_utf8_validate(plain, -1, NULL))
        return FALSE;
    text = g_strstrip(g_strdup(plain));
    /* Reaction notices are short; sentences that merely mention them are not. */
    if(!*text || g_utf8_strlen(text, -1) > 160)
        goto out;
    if(g_regex_match(direct, text, 0, NULL))
    {
        ret = TRUE;
        goto out;
    }
    if(!g_regex_match(named, text, 0, NULL))
        goto out;
    if(g_regex_match(code, text, 0, NULL))
    {
        ret = TRUE;
        goto out;
    }
    /* With a name in front, also require an emoji. */
    for(p = text; *p; p = g_utf8_next_char(p))
    {
        gunichar c = g_utf8_get_char(p);

        if(c > 0xFFFF || g_unichar_type(c) == G_UNICODE_OTHER_SYMBOL)
        {
            ret = TRUE;
            break;
        }
    }
out:
    g_free(text);
    return ret;
}
