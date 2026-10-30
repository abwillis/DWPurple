/*
 * DWPurple - rich conversation history (render widget backend).
 *
 * Alternative to the read-only MLE history, chosen per conversation tab
 * with Preferences > Conversations > History display.  It draws with the
 * Dynamic Windows render widget, which exists on every port, so inline
 * images work on OS/2 as well.
 *
 *  - A history line is a flat UTF-8 string plus pieces: text runs and
 *    images.  An image owns a placeholder ("[image: name]") in the flat
 *    string, so selection and copy treat it like text.
 *  - Image bytes are copied out of libpurple's image store when the line is
 *    added (libpurple may drop a received image right after writing the
 *    message) and decoded once, on first layout, into a cached pixmap.
 *    The cache is keyed by store id, so re-rendering keeps the images.
 *  - A new line is laid out when it is added; everything is laid out again
 *    only when the width changes.  Expose draws only the visible rows,
 *    through a back buffer.
 *  - Selection is line + byte offset on UTF-8 boundaries.  Copy and Select
 *    All are on the right-click menu; Ctrl+C and Ctrl+A also work.
 *    Clicking an image opens the full-size image.
 */
#include "dwpurple.h"

#define HV_PAD          6
#define HV_LINE_GAP     3
#define HV_MAX_IMAGE_W  480
#define HV_MAX_IMAGE_H  320
#define HV_MAX_LINES    2000
#define HV_TRIM_LINES   200
#define HV_SCROLL_W     16
#define HV_REMOTE_MAX   (20 * 1024 * 1024)

typedef struct {
    int id;
    int refs;
    char *name;
    guchar *data;
    gsize len;
    HPIXMAP pixmap;
    gboolean decode_failed;
    char *path;                 /* full image file, written on first open */
    char *url;                  /* remote attachment URL, if any */
    PurpleUtilFetchUrlData *fetch;
    struct _DwpHistView *owner;
} HvImage;

typedef struct {
    gboolean image;
    gboolean link;
    char *url;
    int start, end;             /* byte range in the line's flat text */
    int style;
    HvImage *img;
} HvPiece;

typedef struct {
    GString *flat;
    GArray *pieces;             /* HvPiece */
    int y, h;                   /* content coordinates */
} HvLine;

typedef struct {
    int line;
    int start, end;             /* byte range in the line's flat text */
    int x, y, w, h;             /* content coordinates */
    int style;
    HvImage *img;               /* NULL for text rows */
    const char *url;             /* borrowed from HvPiece */
} HvRow;

typedef struct {
    int line, off;
} HvPos;

struct _DwpHistView {
    HWND box, render, scroll;
    GPtrArray *lines;           /* HvLine */
    GArray *rows;               /* HvRow, in content order */
    GHashTable *images;         /* store id -> HvImage (holds one reference) */
    GHashTable *remote_images;  /* URL -> HvImage (holds one reference) */
    HPIXMAP back;
    int width, height;          /* render widget size */
    int content_h, offset;
    int line_h;
    gboolean follow;
    gboolean selecting, moved, has_sel;
    HvPos anchor, cursor;
    HvImage *press_img;
    HvImage *menu_img;
    const char *press_url;
    const char *menu_url;
};

/* ================================================================= images */

static void image_unref(gpointer p)
{
    HvImage *img = p;

    if(!img || --img->refs > 0)
        return;
    if(img->fetch)
        purple_util_fetch_url_cancel(img->fetch);
    if(img->pixmap)
        dw_pixmap_destroy(img->pixmap);
    g_free(img->name);
    g_free(img->url);
    g_free(img->data);
    g_free(img->path);
    g_free(img);
}

/* Cached copy of a libpurple stored image; NULL if the id is unknown. */
static HvImage *image_get(DwpHistView *hv, int id)
{
    HvImage *img = g_hash_table_lookup(hv->images, GINT_TO_POINTER(id));
    PurpleStoredImage *stored;
    gconstpointer data;
    size_t len;
    const char *name;

    if(img)
        return img;
    stored = purple_imgstore_find_by_id(id);
    if(!stored)
        return NULL;
    data = purple_imgstore_get_data(stored);
    len = purple_imgstore_get_size(stored);
    if(!data || !len)
        return NULL;
    name = purple_imgstore_get_filename(stored);
    img = g_new0(HvImage, 1);
    img->id = id;
    img->refs = 1;
    img->name = g_strdup(name && *name ? name : "image");
    img->data = g_malloc(len);
    memcpy(img->data, data, len);
    img->len = len;
    g_hash_table_insert(hv->images, GINT_TO_POINTER(id), img);
    return img;
}

static HPIXMAP image_pixmap(DwpHistView *hv, HvImage *img);
static void relayout(DwpHistView *hv);

/* Only treat links that are known image attachment forms as images.  Other
 * http(s) links stay ordinary clickable links. */
static gboolean remote_image_url(const char *url)
{
    char *lower;
    gboolean ret;
    const char *q;

    if(!url || (g_ascii_strncasecmp(url, "http://", 7) && g_ascii_strncasecmp(url, "https://", 8)))
        return FALSE;
    lower = g_ascii_strdown(url, -1);
    q = strchr(lower, '?');
    if(q)
        *((char *)q) = '\0';
    ret = strstr(lower, "/views/imgpsh_fullsize") != NULL ||
          g_str_has_suffix(lower, ".gif") || g_str_has_suffix(lower, ".png") ||
          g_str_has_suffix(lower, ".jpg") || g_str_has_suffix(lower, ".jpeg") ||
          g_str_has_suffix(lower, ".webp") || g_str_has_suffix(lower, ".bmp");
    g_free(lower);
    return ret;
}

static char *remote_image_name(const char *url)
{
    char *plain = g_strdup(url), *q = strchr(plain, '?'), *base;

    if(q) *q = '\0';
    base = g_path_get_basename(plain);
    g_free(plain);
    if(!base || !*base || purple_strequal(base, "imgpsh_fullsize"))
    {
        g_free(base);
        return g_strdup("remote-image");
    }
    return base;
}

static void remote_image_done(PurpleUtilFetchUrlData *url_data, gpointer user_data,
                              const gchar *url_text, gsize len, const gchar *error_message)
{
    HvImage *img = user_data;
    DwpHistView *hv = img->owner;

    if(img->fetch == url_data)
        img->fetch = NULL;
    if(!hv)
        return;
    if(!url_text || !len)
    {
        purple_debug_warning(DWP_UI_ID, "Could not fetch inline image %s: %s\n",
                             img->url, error_message ? error_message : "empty response");
        img->decode_failed = TRUE;
    }
    else
    {
        img->data = g_malloc(len);
        memcpy(img->data, url_text, len);
        img->len = len;
        /* Decode now so an HTML/error response cannot reserve image space. */
        if(!image_pixmap(hv, img))
            purple_debug_warning(DWP_UI_ID, "Fetched URL is not a decodable image: %s\n", img->url);
    }
    relayout(hv);
    dw_render_redraw(hv->render);
}

static HvImage *remote_image_get(DwpHistView *hv, const char *url)
{
    HvImage *img = g_hash_table_lookup(hv->remote_images, url);

    if(img)
        return img;
    img = g_new0(HvImage, 1);
    img->id = -1;
    img->refs = 1;
    img->name = remote_image_name(url);
    img->url = g_strdup(url);
    img->owner = hv;
    g_hash_table_insert(hv->remote_images, g_strdup(url), img);
    img->fetch = purple_util_fetch_url_request_len(url, TRUE, "DWPurple/0.1", TRUE,
                                                   NULL, FALSE, HV_REMOTE_MAX,
                                                   remote_image_done, img);
    if(!img->fetch)
        img->decode_failed = TRUE;
    return img;
}

static HPIXMAP image_pixmap(DwpHistView *hv, HvImage *img)
{
    if(!img->pixmap && !img->decode_failed && img->data && img->len)
    {
        img->pixmap = dw_pixmap_new_from_data(hv->render, (const char *)img->data, (int)img->len);
        if(!img->pixmap)
        {
            img->decode_failed = TRUE;
            purple_debug_warning(DWP_UI_ID, "Could not decode image %s for the history view\n", img->name);
        }
    }
    return img->pixmap;
}

/* Scaled size for display; FALSE if the image cannot be shown. */
static gboolean image_size(DwpHistView *hv, HvImage *img, int avail, int *w, int *h)
{
    HPIXMAP pm = image_pixmap(hv, img);
    int iw, ih, maxw = MIN(avail, HV_MAX_IMAGE_W);
    double scale = 1.0;

    if(!pm)
        return FALSE;
    iw = (int)dw_pixmap_get_width(pm);
    ih = (int)dw_pixmap_get_height(pm);
    if(iw <= 0 || ih <= 0)
        return FALSE;
    if(maxw < 16)
        maxw = 16;
    if(iw > maxw)
        scale = (double)maxw / iw;
    if(ih * scale > HV_MAX_IMAGE_H)
        scale = (double)HV_MAX_IMAGE_H / ih;
    *w = MAX(1, (int)(iw * scale));
    *h = MAX(1, (int)(ih * scale));
    return TRUE;
}

static char *safe_file_name(const char *name)
{
    char *base = g_path_get_basename(name && *name ? name : "image");
    char *p;

    for(p = base; *p; p++)
        if(!g_ascii_isalnum(*p) && *p != '.' && *p != '-' && *p != '_')
            *p = '_';
    if(!*base || purple_strequal(base, ".") || purple_strequal(base, ".."))
    {
        g_free(base);
        base = g_strdup("image");
    }
    return base;
}

/* Write the image to <config>/images once and open it with the system viewer. */
static void image_open(HvImage *img)
{
    char *uri;

    if(!img)
        return;
    if(img->url)
    {
        dwp_open_link(img->url);
        return;
    }
    if(!img->path)
    {
        char *dir = g_build_filename(purple_user_dir(), "images", NULL);
        char *safe = safe_file_name(img->name);
        char *file = g_strdup_printf("%d-%s", img->id, safe);
        char *path = g_build_filename(dir, file, NULL);

        if(g_mkdir_with_parents(dir, 0700) == 0 &&
           g_file_set_contents(path, (const char *)img->data, (gssize)img->len, NULL))
            img->path = path;
        else
        {
            purple_debug_error(DWP_UI_ID, "Could not save %s\n", path);
            g_free(path);
        }
        g_free(file);
        g_free(safe);
        g_free(dir);
        if(!img->path)
            return;
    }
    uri = g_filename_to_uri(img->path, NULL, NULL);
    if(uri)
    {
        dw_browse(uri);
        g_free(uri);
    }
}

/* ================================================================== lines */

static void line_free(gpointer p)
{
    HvLine *l = p;
    guint z;

    for(z = 0; z < l->pieces->len; z++)
    {
        HvPiece *pc = &g_array_index(l->pieces, HvPiece, z);

        if(pc->img)
            image_unref(pc->img);
        g_free(pc->url);
    }
    g_array_free(l->pieces, TRUE);
    g_string_free(l->flat, TRUE);
    g_free(l);
}

static void line_add_text_link(HvLine *l, const char *text, int style, const char *url)
{
    HvPiece pc;
    char *fixed = NULL;

    if(!text || !*text)
        return;
    if(!g_utf8_validate(text, -1, NULL))
        text = fixed = purple_utf8_salvage(text);
    memset(&pc, 0, sizeof(pc));
    pc.start = (int)l->flat->len;
    g_string_append(l->flat, text);
    pc.end = (int)l->flat->len;
    pc.style = style;
    pc.link = url && *url;
    pc.url = pc.link ? g_strdup(url) : NULL;
    g_array_append_val(l->pieces, pc);
    g_free(fixed);
}

static void line_add_text(HvLine *l, const char *text, int style)
{
    line_add_text_link(l, text, style, NULL);
}

static void line_add_image(HvLine *l, HvImage *img, int style)
{
    HvPiece pc;

    memset(&pc, 0, sizeof(pc));
    pc.image = TRUE;
    pc.style = style;
    pc.img = img;
    img->refs++;
    pc.start = (int)l->flat->len;
    g_string_append_printf(l->flat, "[image: %s]", img->name);
    pc.end = (int)l->flat->len;
    g_array_append_val(l->pieces, pc);
}

static int img_tag_id(const char *tag)
{
    static GRegex *re = NULL;
    GMatchInfo *mi = NULL;
    int id = 0;

    if(!re)
        re = g_regex_new("\\bid\\s*=\\s*[\"']?([0-9]+)", G_REGEX_CASELESS, 0, NULL);
    if(re && g_regex_match(re, tag, 0, &mi))
    {
        char *num = g_match_info_fetch(mi, 1);

        id = num ? atoi(num) : 0;
        g_free(num);
    }
    g_match_info_free(mi);
    return id;
}

/* Attribute value from a tag, newly allocated. */
static char *tag_attribute(const char *tag, const char *name)
{
    char *pattern = g_strdup_printf("\\b%s\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)'|([^\\s>]+))", name);
    GRegex *re = g_regex_new(pattern, G_REGEX_CASELESS, 0, NULL);
    GMatchInfo *mi = NULL;
    char *ret = NULL;
    int z;

    g_free(pattern);
    if(re && g_regex_match(re, tag, 0, &mi))
        for(z = 1; z <= 3 && !ret; z++)
        {
            int a = -1, b = -1;
            if(g_match_info_fetch_pos(mi, z, &a, &b) && a >= 0)
                ret = g_strndup(tag + a, b - a);
        }
    if(mi) g_match_info_free(mi);
    if(re) g_regex_unref(re);
    return ret;
}

/* Remove non-display metadata from MS Teams messages.  The Teams plugin
 * commonly emits a visible "Click here to view full version" anchor followed
 * by the real attachment message.  Rich history turns that attachment anchor
 * into an image instead of printing its markup. */
static void line_add_markup(DwpHistView *hv, HvLine *l, const char *html,
                            gboolean emoji_codes, int style)
{
    static GRegex *tag_re = NULL;
    const char *p = html ? html : "";
    const char *link = NULL;
    char *link_owned = NULL;
    gboolean link_is_image = FALSE;

    if(!tag_re)
        tag_re = g_regex_new("<[^>]+>", G_REGEX_CASELESS, 0, NULL);
    while(*p)
    {
        GMatchInfo *mi = NULL;
        int a, b;
        char *seg, *text, *tag, *href = NULL;

        if(!tag_re || !g_regex_match(tag_re, p, 0, &mi))
        {
            text = dwp_html_to_text(p, emoji_codes);
            line_add_text_link(l, text, style, link);
            g_free(text);
            if(mi) g_match_info_free(mi);
            break;
        }
        g_match_info_fetch_pos(mi, 0, &a, &b);
        g_match_info_free(mi);
        seg = g_strndup(p, a);
        text = dwp_html_to_text(seg, emoji_codes);
        if(!link_is_image)
            line_add_text_link(l, text, style, link);
        g_free(text); g_free(seg);
        tag = g_strndup(p + a, b - a);

        if(g_ascii_strncasecmp(tag, "<img", 4) == 0)
        {
            int id = img_tag_id(tag);
            HvImage *img = id > 0 ? image_get(hv, id) : NULL;
            if(img)
                line_add_image(l, img, style);
            else
            {
                char *alt = tag_attribute(tag, "alt");
                if(!alt || !*alt) { g_free(alt); alt = tag_attribute(tag, "title"); }
                line_add_text_link(l, alt && *alt ? alt : "[image]", style, NULL);
                g_free(alt);
            }
        }
        else if(g_ascii_strncasecmp(tag, "<a", 2) == 0)
        {
            href = tag_attribute(tag, "href");
            g_free(link_owned);
            link_owned = href;
            link = link_owned;
            link_is_image = remote_image_url(link);
            if(link_is_image)
            {
                HvImage *remote = remote_image_get(hv, link);
                if(remote)
                    line_add_image(l, remote, style);
            }
        }
        else if(g_ascii_strncasecmp(tag, "</a", 3) == 0)
        {
            g_free(link_owned); link_owned = NULL; link = NULL; link_is_image = FALSE;
        }
        else if(g_ascii_strncasecmp(tag, "<br", 3) == 0 || g_ascii_strncasecmp(tag, "</p", 3) == 0)
            line_add_text(l, "\n", style);
        g_free(tag);
        p += b;
    }
    g_free(link_owned);
}
/* ================================================================= layout */

static int text_w(DwpHistView *hv, const char *s, int len)
{
    char *t;
    int w = 0, h = 0;

    if(len <= 0)
        return 0;
    t = g_strndup(s, len);
    dw_font_text_extents_get(hv->render, 0, t, &w, &h);
    g_free(t);
    return w;
}

static int text_avail(DwpHistView *hv)
{
    return MAX(hv->width - 2 * HV_PAD, 40);
}

static void add_row(DwpHistView *hv, int line, int start, int end, int y, int w, int h,
                    HvImage *img, const char *url, int style)
{
    HvRow r;

    r.line = line;
    r.start = start;
    r.end = end;
    r.x = HV_PAD;
    r.y = y;
    r.w = w;
    r.h = h;
    r.style = style;
    r.img = img;
    r.url = url;
    g_array_append_val(hv->rows, r);
}

/* Wrap flat[s, e) into rows starting at y; returns the y below them. */
static int wrap_text(DwpHistView *hv, int li, const char *flat, int s, int e, int y, int style)
{
    int avail = text_avail(hv), pos = s;

    while(pos < e)
    {
        const char *nl = memchr(flat + pos, '\n', (size_t)(e - pos));
        int pe = nl ? (int)(nl - flat) : e;
        int ls = pos;

        if(ls == pe)
        {
            add_row(hv, li, ls, ls, y, avail, hv->line_h, NULL, NULL, style);
            y += hv->line_h;
        }
        while(ls < pe)
        {
            int cut = ls, cur = ls;

            /* Grow the row a word at a time. */
            while(cur < pe)
            {
                int next = cur;

                while(next < pe && flat[next] == ' ')
                    next++;
                while(next < pe && flat[next] != ' ')
                    next++;
                if(text_w(hv, flat + ls, next - ls) > avail)
                    break;
                cut = cur = next;
            }
            if(cut == ls)
            {
                /* A single word wider than the row: break it by characters. */
                int c = ls;

                while(c < pe)
                {
                    int n = (int)(g_utf8_next_char(flat + c) - flat);

                    if(n > pe)
                        n = pe;
                    if(c > ls && text_w(hv, flat + ls, n - ls) > avail)
                        break;
                    c = n;
                }
                cut = c;
            }
            add_row(hv, li, ls, cut, y, avail, hv->line_h, NULL, NULL, style);
            y += hv->line_h;
            ls = cut;
            while(ls < pe && flat[ls] == ' ')
                ls++;
        }
        pos = pe + 1;
    }
    return y;
}

static void layout_line(DwpHistView *hv, int li, int y)
{
    HvLine *l = g_ptr_array_index(hv->lines, li);
    guint first = hv->rows->len, z;
    int row_y = y;

    l->y = y;
    for(z = 0; z < l->pieces->len; z++)
    {
        HvPiece *pc = &g_array_index(l->pieces, HvPiece, z);
        int w, h;

        if(pc->image && image_size(hv, pc->img, text_avail(hv), &w, &h))
        {
            add_row(hv, li, pc->start, pc->end, row_y, w, h, pc->img, NULL, pc->style);
            row_y += h + HV_LINE_GAP;
        }
        else
        {
            guint before = hv->rows->len, q;
            row_y = wrap_text(hv, li, l->flat->str, pc->start, pc->end, row_y, pc->style);
            for(q = before; q < hv->rows->len; q++)
            {
                HvRow *r = &g_array_index(hv->rows, HvRow, q);
                r->url = pc->url;
                if(pc->url)
                    r->w = text_w(hv, l->flat->str + r->start, r->end - r->start);
            }
        }
    }
    if(hv->rows->len == first)
    {
        add_row(hv, li, 0, 0, row_y, text_avail(hv), hv->line_h, NULL, NULL, DWP_HIST_NORMAL);
        row_y += hv->line_h;
    }
    l->h = row_y - y;
}

static void measure_font(DwpHistView *hv)
{
    int w = 0, h = 0;

    dw_font_text_extents_get(hv->render, 0, "Ag", &w, &h);
    hv->line_h = h > 0 ? h + 2 : 16;
}

static void update_scroll(DwpHistView *hv)
{
    int max = MAX(0, hv->content_h - hv->height);

    if(hv->follow)
        hv->offset = max;
    hv->offset = CLAMP(hv->offset, 0, max);
    dw_scrollbar_set_range(hv->scroll, (unsigned int)MAX(hv->content_h, 1), (unsigned int)MAX(hv->height, 1));
    dw_scrollbar_set_pos(hv->scroll, (unsigned int)hv->offset);
}

static void relayout(DwpHistView *hv)
{
    int y = HV_PAD;
    guint z;

    g_array_set_size(hv->rows, 0);
    if(hv->width <= 0)
        return;
    measure_font(hv);
    for(z = 0; z < hv->lines->len; z++)
    {
        HvLine *l;

        layout_line(hv, (int)z, y);
        l = g_ptr_array_index(hv->lines, z);
        y = l->y + l->h + HV_LINE_GAP;
    }
    hv->content_h = y + HV_PAD;
    update_scroll(hv);
}

/* ============================================================== selection */

static int pos_cmp(HvPos a, HvPos b)
{
    if(a.line != b.line)
        return a.line < b.line ? -1 : 1;
    return a.off < b.off ? -1 : (a.off > b.off ? 1 : 0);
}

static void sel_range(DwpHistView *hv, HvPos *a, HvPos *b)
{
    if(pos_cmp(hv->anchor, hv->cursor) <= 0)
    {
        *a = hv->anchor;
        *b = hv->cursor;
    }
    else
    {
        *a = hv->cursor;
        *b = hv->anchor;
    }
}

/* Index of the row at content y (clamped), -1 if there are no rows. */
static int row_at_y(DwpHistView *hv, int cy)
{
    int lo = 0, hi = (int)hv->rows->len - 1;

    if(hi < 0)
        return -1;
    while(lo < hi)
    {
        int mid = (lo + hi + 1) / 2;

        if(g_array_index(hv->rows, HvRow, mid).y <= cy)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

static HvPos pos_at(DwpHistView *hv, int x, int y)
{
    HvPos p = { 0, 0 };
    int ri = row_at_y(hv, y + hv->offset);
    HvRow *r;
    HvLine *l;
    int c, prev = 0;

    if(ri < 0)
        return p;
    r = &g_array_index(hv->rows, HvRow, ri);
    p.line = r->line;
    if(y + hv->offset < r->y)
    {
        p.off = r->start;
        return p;
    }
    if(y + hv->offset >= r->y + r->h + HV_LINE_GAP && ri == (int)hv->rows->len - 1)
    {
        l = g_ptr_array_index(hv->lines, r->line);
        p.off = (int)l->flat->len;
        return p;
    }
    if(r->img)
    {
        p.off = x < r->x + r->w / 2 ? r->start : r->end;
        return p;
    }
    l = g_ptr_array_index(hv->lines, r->line);
    for(c = r->start; c < r->end; )
    {
        int n = (int)(g_utf8_next_char(l->flat->str + c) - l->flat->str);
        int wn;

        if(n > r->end)
            n = r->end;
        wn = text_w(hv, l->flat->str + r->start, n - r->start);
        if(r->x + wn > x)
        {
            p.off = (x - (r->x + prev) < (wn - prev) / 2) ? c : n;
            return p;
        }
        prev = wn;
        c = n;
    }
    p.off = r->end;
    return p;
}

static HvImage *image_at(DwpHistView *hv, int x, int y)
{
    int ri = row_at_y(hv, y + hv->offset);
    HvRow *r;

    if(ri < 0)
        return NULL;
    r = &g_array_index(hv->rows, HvRow, ri);
    if(r->img && x >= r->x && x < r->x + r->w &&
       y + hv->offset >= r->y && y + hv->offset < r->y + r->h)
        return r->img;
    return NULL;
}

static const char *link_at(DwpHistView *hv, int x, int y)
{
    int ri = row_at_y(hv, y + hv->offset);
    HvRow *r;
    if(ri < 0) return NULL;
    r = &g_array_index(hv->rows, HvRow, ri);
    if(r->url && x >= r->x && x < r->x + r->w &&
       y + hv->offset >= r->y && y + hv->offset < r->y + r->h)
        return r->url;
    return NULL;
}

static char *selection_text(DwpHistView *hv)
{
#if defined(_WIN32) || defined(__OS2__)
    static const char *const eol = "\r\n";
#else
    static const char *const eol = "\n";
#endif
    GString *out = g_string_new(NULL);
    HvPos a, b;
    int li;

    if(!hv->has_sel || !hv->lines->len)
        return g_string_free(out, FALSE);
    sel_range(hv, &a, &b);
    for(li = a.line; li <= b.line && li < (int)hv->lines->len; li++)
    {
        HvLine *l = g_ptr_array_index(hv->lines, li);
        int len = (int)l->flat->len;
        int s = li == a.line ? CLAMP(a.off, 0, len) : 0;
        int e = li == b.line ? CLAMP(b.off, 0, len) : len;
        const char *p;

        if(li > a.line)
            g_string_append(out, eol);
        for(p = l->flat->str + s; p < l->flat->str + e; p++)
        {
            if(*p == '\n')
                g_string_append(out, eol);
            else
                g_string_append_c(out, *p);
        }
    }
    return g_string_free(out, FALSE);
}

static void copy_selection(DwpHistView *hv)
{
    char *text = selection_text(hv);

    if(*text)
        dw_clipboard_set_text(text, (int)strlen(text));
    g_free(text);
}

static void select_all(DwpHistView *hv)
{
    HvLine *last;

    if(!hv->lines->len)
        return;
    last = g_ptr_array_index(hv->lines, hv->lines->len - 1);
    hv->anchor.line = 0;
    hv->anchor.off = 0;
    hv->cursor.line = (int)hv->lines->len - 1;
    hv->cursor.off = (int)last->flat->len;
    hv->has_sel = TRUE;
    dw_render_redraw(hv->render);
}

/* ================================================================ drawing */

static unsigned long style_color(int style)
{
    switch(style)
    {
        case DWP_HIST_SYSTEM: return DW_RGB(96, 96, 96);
        case DWP_HIST_ERROR:  return DW_RGB(170, 0, 0);
        default:              return DW_CLR_BLACK;
    }
}

static void draw_row(DwpHistView *hv, HvRow *r, gboolean sel, HvPos a, HvPos b)
{
    HvLine *l = g_ptr_array_index(hv->lines, r->line);
    int yy = r->y - hv->offset;

    if(sel && r->line >= a.line && r->line <= b.line)
    {
        int len = (int)l->flat->len;
        int s = r->line == a.line ? a.off : 0;
        int e = r->line == b.line ? b.off : len;

        s = MAX(s, r->start);
        e = MIN(e, r->end);
        dw_color_foreground_set(DW_RGB(173, 214, 255));
        if(r->img)
        {
            if(s <= r->start && e >= r->end && r->start < r->end)
                dw_draw_rect(0, hv->back, TRUE, r->x - 2, yy - 2, r->w + 4, r->h + 4);
        }
        else if(s < e)
        {
            int x1 = r->x + text_w(hv, l->flat->str + r->start, s - r->start);
            int x2 = r->x + text_w(hv, l->flat->str + r->start, e - r->start);

            dw_draw_rect(0, hv->back, TRUE, x1, yy, MAX(x2 - x1, 1), r->h);
        }
    }
    if(r->img)
    {
        HPIXMAP pm = image_pixmap(hv, r->img);

        if(pm)
        {
            int iw = (int)dw_pixmap_get_width(pm), ih = (int)dw_pixmap_get_height(pm);

            if(dw_pixmap_stretch_bitblt(0, hv->back, r->x, yy, r->w, r->h,
                                        0, pm, 0, 0, iw, ih) != DW_ERROR_NONE)
                dw_pixmap_bitblt(0, hv->back, r->x, yy, MIN(r->w, iw), MIN(r->h, ih), 0, pm, 0, 0);
        }
    }
    else if(r->end > r->start)
    {
        char *t = g_strndup(l->flat->str + r->start, r->end - r->start);

        dw_color_foreground_set(r->url ? DW_RGB(0, 76, 170) : style_color(r->style));
        dw_color_background_set(DW_CLR_DEFAULT);
        dw_draw_text(0, hv->back, r->x, yy, t);
        g_free(t);
    }
}

static int DWSIGNAL hv_expose(HWND w, DWExpose *exp, void *data)
{
    DwpHistView *hv = data;
    HvPos a = { 0, 0 }, b = { 0, 0 };
    int ri;

    (void)w;
    (void)exp;
    if(hv->width <= 0 || hv->height <= 0)
    {
        /* No configure event yet on some ports: ask for the size. */
        long px = 0, py = 0;
        unsigned long cw = 0, ch = 0;

        dw_window_get_pos_size(hv->render, &px, &py, &cw, &ch);
        if(cw > 0 && ch > 0)
        {
            hv->width = (int)cw;
            hv->height = (int)ch;
            relayout(hv);
        }
        if(hv->width <= 0 || hv->height <= 0)
            return TRUE;
    }
    if(!hv->back)
    {
        char *font = dw_window_get_font(hv->render);

        hv->back = dw_pixmap_new(hv->render, (unsigned long)hv->width, (unsigned long)hv->height,
                                 (int)dw_color_depth_get());
        if(!hv->back)
        {
            if(font)
                dw_free(font);
            return TRUE;
        }
        if(font)
        {
            dw_pixmap_set_font(hv->back, font);
            dw_free(font);
        }
    }
    dw_color_foreground_set(DW_CLR_WHITE);
    dw_draw_rect(0, hv->back, TRUE, 0, 0, hv->width, hv->height);
    if(hv->has_sel)
        sel_range(hv, &a, &b);
    for(ri = MAX(row_at_y(hv, hv->offset), 0); ri < (int)hv->rows->len; ri++)
    {
        HvRow *r = &g_array_index(hv->rows, HvRow, ri);

        if(r->y >= hv->offset + hv->height)
            break;
        if(r->y + r->h < hv->offset)
            continue;
        draw_row(hv, r, hv->has_sel, a, b);
    }
    dw_pixmap_bitblt(hv->render, 0, 0, 0, hv->width, hv->height, 0, hv->back, 0, 0);
    return TRUE;
}

static int DWSIGNAL hv_configure(HWND w, int width, int height, void *data)
{
    DwpHistView *hv = data;
    gboolean width_changed = width != hv->width;

    (void)w;
    if(width == hv->width && height == hv->height)
        return TRUE;
    hv->width = width;
    hv->height = height;
    if(hv->back)
    {
        dw_pixmap_destroy(hv->back);
        hv->back = NULL;
    }
    if(width_changed)
        relayout(hv);
    else
        update_scroll(hv);
    dw_render_redraw(hv->render);
    return TRUE;
}

/* ================================================================= input */

static void scroll_to(DwpHistView *hv, int offset)
{
    int max = MAX(0, hv->content_h - hv->height);

    hv->offset = CLAMP(offset, 0, max);
    hv->follow = hv->offset >= max;
    dw_scrollbar_set_pos(hv->scroll, (unsigned int)hv->offset);
    dw_render_redraw(hv->render);
}

static int DWSIGNAL hv_scrolled(HWND w, int value, void *data)
{
    DwpHistView *hv = data;
    int max = MAX(0, hv->content_h - hv->height);

    (void)w;
    hv->offset = CLAMP(value, 0, max);
    hv->follow = hv->offset >= max;
    dw_render_redraw(hv->render);
    return TRUE;
}

static void menu_copy(gpointer a, gpointer b, const char *s)
{
    (void)b; (void)s;
    copy_selection(a);
}

static void menu_select_all(gpointer a, gpointer b, const char *s)
{
    (void)b; (void)s;
    select_all(a);
}

static void menu_open_image(gpointer a, gpointer b, const char *s)
{
    DwpHistView *hv = a;

    (void)b; (void)s;
    image_open(hv->menu_img);
}

static void menu_open_link(gpointer a, gpointer b, const char *s)
{
    DwpHistView *hv = a;

    (void)b; (void)s;
    if(hv->menu_url)
        dwp_open_link(hv->menu_url);
}

static int DWSIGNAL hv_button_press(HWND w, int x, int y, int button, void *data)
{
    DwpHistView *hv = data;

    (void)w;
    dwp_note_activity();
    dw_window_set_focus(hv->render);
    if(button == DW_BUTTON1_MASK)
    {
        hv->anchor = hv->cursor = pos_at(hv, x, y);
        hv->selecting = TRUE;
        hv->moved = FALSE;
        hv->has_sel = FALSE;
        hv->press_img = image_at(hv, x, y);
        hv->press_url = link_at(hv, x, y);
        dw_render_redraw(hv->render);
    }
    else
    {
        HMENUI menu;

        hv->menu_img = image_at(hv, x, y);
        hv->menu_url = link_at(hv, x, y);
        dwp_actions_reset();
        menu = dw_menu_new(0);
        if(hv->menu_img)
            dwp_menu_add(menu, "~Open Image", menu_open_image, hv, NULL, NULL);
        if(hv->menu_url)
            dwp_menu_add(menu, "Open ~Link", menu_open_link, hv, NULL, NULL);
        dwp_menu_add(menu, "~Copy", hv->has_sel ? menu_copy : NULL, hv, NULL, NULL);
        dwp_menu_add(menu, "Select ~All", menu_select_all, hv, NULL, NULL);
        dwp_menu_popup_here(&menu, hv->render);
    }
    return TRUE;
}

static int DWSIGNAL hv_motion(HWND w, int x, int y, int mask, void *data)
{
    DwpHistView *hv = data;
    HvPos p;

    (void)w;
    (void)mask;
    if(!hv->selecting)
        return FALSE;
    /* Drag above or below the view: scroll. */
    if(y < 0)
        scroll_to(hv, hv->offset - hv->line_h);
    else if(y > hv->height)
        scroll_to(hv, hv->offset + hv->line_h);
    p = pos_at(hv, x, CLAMP(y, 0, hv->height));
    if(pos_cmp(p, hv->cursor) != 0)
    {
        hv->cursor = p;
        hv->moved = TRUE;
        hv->has_sel = pos_cmp(hv->anchor, hv->cursor) != 0;
        dw_render_redraw(hv->render);
    }
    return TRUE;
}

static int DWSIGNAL hv_button_release(HWND w, int x, int y, int button, void *data)
{
    DwpHistView *hv = data;
    HvImage *img;

    (void)w;
    (void)button;
    if(!hv->selecting)
        return FALSE;
    hv->selecting = FALSE;
    img = hv->press_img;
    hv->press_img = NULL;
    if(!hv->moved && img && image_at(hv, x, y) == img)
        image_open(img);
    else if(!hv->moved && hv->press_url && link_at(hv, x, y) &&
            purple_strequal(hv->press_url, link_at(hv, x, y)))
        dwp_open_link(hv->press_url);
    hv->press_url = NULL;
    return TRUE;
}

static int DWSIGNAL hv_key(HWND w, char ch, int vk, int state, void *data, char *utf8)
{
    DwpHistView *hv = data;

    (void)w;
    (void)utf8;
    if((state & KC_CTRL) && (ch == 0x03 || vk == 'c' || vk == 'C'))
    {
        copy_selection(hv);
        return TRUE;
    }
    if((state & KC_CTRL) && (ch == 0x01 || vk == 'a' || vk == 'A'))
    {
        select_all(hv);
        return TRUE;
    }
    if(vk == VK_PRIOR)
        scroll_to(hv, hv->offset - MAX(hv->height - hv->line_h, hv->line_h));
    else if(vk == VK_NEXT)
        scroll_to(hv, hv->offset + MAX(hv->height - hv->line_h, hv->line_h));
    else if(vk == VK_UP)
        scroll_to(hv, hv->offset - hv->line_h);
    else if(vk == VK_DOWN)
        scroll_to(hv, hv->offset + hv->line_h);
    else if(vk == VK_HOME && (state & KC_CTRL))
        scroll_to(hv, 0);
    else if(vk == VK_END && (state & KC_CTRL))
        scroll_to(hv, hv->content_h);
    else
        return FALSE;
    return TRUE;
}

/* ================================================================= public */

DwpHistView *dwp_histview_new(void)
{
    DwpHistView *hv = g_new0(DwpHistView, 1);

    hv->lines = g_ptr_array_new_with_free_func(line_free);
    hv->rows = g_array_new(FALSE, FALSE, sizeof(HvRow));
    hv->images = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, image_unref);
    hv->remote_images = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, image_unref);
    hv->follow = TRUE;
    hv->line_h = 16;
    hv->box = dw_box_new(DW_HORZ, 0);
    hv->render = dw_render_new(0);
    hv->scroll = dw_scrollbar_new(TRUE, 0);
    dw_box_pack_start(hv->box, hv->render, 300, 200, TRUE, TRUE, 0);
    dw_box_pack_start(hv->box, hv->scroll, HV_SCROLL_W, 200, FALSE, TRUE, 0);
    dw_signal_connect(hv->render, DW_SIGNAL_EXPOSE, DW_SIGNAL_FUNC(hv_expose), hv);
    dw_signal_connect(hv->render, DW_SIGNAL_CONFIGURE, DW_SIGNAL_FUNC(hv_configure), hv);
    dw_signal_connect(hv->render, DW_SIGNAL_BUTTON_PRESS, DW_SIGNAL_FUNC(hv_button_press), hv);
    dw_signal_connect(hv->render, DW_SIGNAL_BUTTON_RELEASE, DW_SIGNAL_FUNC(hv_button_release), hv);
    dw_signal_connect(hv->render, DW_SIGNAL_MOTION_NOTIFY, DW_SIGNAL_FUNC(hv_motion), hv);
    dw_signal_connect(hv->render, DW_SIGNAL_KEY_PRESS, DW_SIGNAL_FUNC(hv_key), hv);
    dw_signal_connect(hv->scroll, DW_SIGNAL_VALUE_CHANGED, DW_SIGNAL_FUNC(hv_scrolled), hv);
    return hv;
}

HWND dwp_histview_widget(DwpHistView *hv)
{
    return hv ? hv->box : 0;
}

/* Drop the oldest lines and cached images no line uses any more. */
static void trim(DwpHistView *hv)
{
    GHashTableIter iter;
    gpointer key, value;

    g_ptr_array_remove_range(hv->lines, 0, HV_TRIM_LINES);
    hv->has_sel = hv->selecting = FALSE;
    hv->press_img = hv->menu_img = NULL;
    hv->press_url = hv->menu_url = NULL;
    g_hash_table_iter_init(&iter, hv->images);
    while(g_hash_table_iter_next(&iter, &key, &value))
        if(((HvImage *)value)->refs == 1)
            g_hash_table_iter_remove(&iter);
    relayout(hv);
}

void dwp_histview_append(DwpHistView *hv, const char *prefix, const char *html,
                         gboolean emoji_codes, int style)
{
    HvLine *l;

    if(!hv)
        return;
    l = g_new0(HvLine, 1);
    l->flat = g_string_new(NULL);
    l->pieces = g_array_new(FALSE, FALSE, sizeof(HvPiece));
    line_add_text(l, prefix, style);
    line_add_markup(hv, l, html, emoji_codes, style);
    g_ptr_array_add(hv->lines, l);
    if(hv->lines->len > HV_MAX_LINES)
        trim(hv);
    else if(hv->width > 0)
    {
        int y = hv->lines->len > 1 ? hv->content_h - HV_PAD : HV_PAD;

        if(hv->lines->len == 1)
            measure_font(hv);
        layout_line(hv, (int)hv->lines->len - 1, y);
        hv->content_h = l->y + l->h + HV_LINE_GAP + HV_PAD;
        update_scroll(hv);
    }
    dw_render_redraw(hv->render);
}

void dwp_histview_clear(DwpHistView *hv)
{
    if(!hv)
        return;
    g_ptr_array_set_size(hv->lines, 0);
    g_array_set_size(hv->rows, 0);
    hv->content_h = 0;
    hv->offset = 0;
    hv->follow = TRUE;
    hv->has_sel = hv->selecting = FALSE;
    hv->press_img = hv->menu_img = NULL;
    hv->press_url = hv->menu_url = NULL;
    update_scroll(hv);
    dw_render_redraw(hv->render);
}

/* The widgets are destroyed with their window; this releases the data. */
void dwp_histview_free(DwpHistView *hv)
{
    if(!hv)
        return;
    g_ptr_array_free(hv->lines, TRUE);
    g_array_free(hv->rows, TRUE);
    {
        GHashTableIter iter;
        gpointer key, value;
        g_hash_table_iter_init(&iter, hv->remote_images);
        while(g_hash_table_iter_next(&iter, &key, &value))
            ((HvImage *)value)->owner = NULL;
    }
    g_hash_table_destroy(hv->remote_images);
    g_hash_table_destroy(hv->images);
    if(hv->back)
        dw_pixmap_destroy(hv->back);
    g_free(hv);
}

