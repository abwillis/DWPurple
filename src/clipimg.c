/*
 * DWPurple - read an image from the clipboard.
 *
 * Dynamic Windows only exposes text on the clipboard
 * (dw_clipboard_get_text/set_text), so images need help:
 *
 *   GTK 3 builds   the GTK clipboard, looked up at run time with dlsym()
 *                  (DWPurple does not link GTK directly): a copied image
 *                  (screenshot, "Copy Image" in a browser) is encoded as
 *                  PNG; copied image files (file manager) are read from disk.
 *   everywhere     clipboard text holding a file:// URI of an image file.
 *
 * With prefer_text, ordinary text on the clipboard wins: applications such
 * as office suites and browsers put both text and a picture of it there,
 * and Ctrl+V of such a selection should paste the text.
 *
 * OS/2 and Windows bitmaps on the clipboard (CF_BITMAP / CF_DIB) are not
 * read; copying the image file itself works there through the text path
 * when the file manager puts a file:// URI on the clipboard.
 */
#if defined(__linux__) || defined(__FreeBSD__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif

#include "dwpurple.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <glib/gstdio.h>

#define CLIP_MAX_BYTES (20 * 1024 * 1024)

static gboolean has_image_ext(const char *name)
{
    static const char *const exts[] = { ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".webp", NULL };
    char *lower;
    gboolean ret = FALSE;
    int z;

    if(!name)
        return FALSE;
    lower = g_ascii_strdown(name, -1);
    for(z = 0; exts[z] && !ret; z++)
        ret = g_str_has_suffix(lower, exts[z]);
    g_free(lower);
    return ret;
}

/* Read a local image file named by a file:// URI. */
static gboolean load_uri(const char *uri, guchar **data, gsize *len, char **name)
{
    char *path, *contents = NULL;
    gsize size = 0;
    GStatBuf st;

    if(!uri || g_ascii_strncasecmp(uri, "file:", 5) != 0)
        return FALSE;
    path = g_filename_from_uri(uri, NULL, NULL);
    if(!path)
        return FALSE;
    if(!has_image_ext(path) || g_stat(path, &st) != 0 || !S_ISREG(st.st_mode) ||
       st.st_size <= 0 || st.st_size > CLIP_MAX_BYTES ||
       !g_file_get_contents(path, &contents, &size, NULL) || !size)
    {
        g_free(contents);
        g_free(path);
        return FALSE;
    }
    *data = (guchar *)contents;
    *len = size;
    *name = g_path_get_basename(path);
    g_free(path);
    return TRUE;
}

/* Text clipboard: one or more URIs (text/uri-list style), first image wins. */
static gboolean from_text(guchar **data, gsize *len, char **name)
{
    char *text = dw_clipboard_get_text(), **lines;
    gboolean ret = FALSE;
    int z;

    if(!text)
        return FALSE;
    lines = g_strsplit_set(text, "\r\n", -1);
    dw_free(text);
    for(z = 0; lines[z] && !ret; z++)
    {
        char *line = g_strstrip(lines[z]);

        if(*line && *line != '#')
            ret = load_uri(line, data, len, name);
    }
    g_strfreev(lines);
    return ret;
}

#if defined(__GLIBC__) && !defined(_WIN32) && !defined(__OS2__) && !defined(__APPLE__)
#include <dlfcn.h>

typedef void *(*AtomInternFn)(const char *, gboolean);
typedef void *(*ClipGetFn)(void *);
typedef void *(*WaitImageFn)(void *);
typedef char **(*WaitUrisFn)(void *);
typedef gboolean (*SaveBufferFn)(void *, char **, gsize *, const char *, GError **, ...);

static gboolean from_gtk(guchar **data, gsize *len, char **name)
{
    AtomInternFn atom_intern;
    ClipGetFn clip_get;
    WaitImageFn wait_image;
    WaitUrisFn wait_uris;
    SaveBufferFn save_buffer;
    void *clipboard, *pixbuf;
    gboolean ret = FALSE;

    *(void **)(&atom_intern) = dlsym(RTLD_DEFAULT, "gdk_atom_intern");
    *(void **)(&clip_get) = dlsym(RTLD_DEFAULT, "gtk_clipboard_get");
    *(void **)(&wait_image) = dlsym(RTLD_DEFAULT, "gtk_clipboard_wait_for_image");
    *(void **)(&wait_uris) = dlsym(RTLD_DEFAULT, "gtk_clipboard_wait_for_uris");
    *(void **)(&save_buffer) = dlsym(RTLD_DEFAULT, "gdk_pixbuf_save_to_buffer");
    if(!atom_intern || !clip_get)
        return FALSE;
    clipboard = clip_get(atom_intern("CLIPBOARD", FALSE));
    if(!clipboard)
        return FALSE;

    /* Image data (screenshots, "Copy Image"). */
    if(wait_image && save_buffer && (pixbuf = wait_image(clipboard)))
    {
        char *buf = NULL;
        gsize size = 0;
        GError *error = NULL;

        if(save_buffer(pixbuf, &buf, &size, "png", &error, NULL) && buf && size)
        {
            *data = (guchar *)buf;
            *len = size;
            *name = g_strdup("pasted-image.png");
            ret = TRUE;
        }
        else
        {
            purple_debug_warning(DWP_UI_ID, "Could not encode clipboard image: %s\n",
                                 error ? error->message : "unknown error");
            g_free(buf);
        }
        if(error)
            g_error_free(error);
        g_object_unref(pixbuf);
        if(ret)
            return TRUE;
    }

    /* Copied image files. */
    if(wait_uris)
    {
        char **uris = wait_uris(clipboard);
        int z;

        for(z = 0; uris && uris[z] && !ret; z++)
            ret = load_uri(uris[z], data, len, name);
        g_strfreev(uris);
    }
    return ret;
}
#else
static gboolean from_gtk(guchar **data, gsize *len, char **name)
{
    (void)data;
    (void)len;
    (void)name;
    return FALSE;
}
#endif

/* TRUE when the clipboard holds text that is not just file URIs. */
static gboolean has_plain_text(void)
{
    char *text = dw_clipboard_get_text(), **lines;
    gboolean ret = FALSE;
    int z;

    if(!text)
        return FALSE;
    lines = g_strsplit_set(text, "\r\n", -1);
    dw_free(text);
    for(z = 0; lines[z] && !ret; z++)
    {
        char *line = g_strstrip(lines[z]);

        if(*line && *line != '#' && g_ascii_strncasecmp(line, "file:", 5) != 0)
            ret = TRUE;
    }
    g_strfreev(lines);
    return ret;
}

gboolean dwp_clipboard_get_image(gboolean prefer_text, guchar **data, gsize *len, char **name)
{
    g_return_val_if_fail(data && len && name, FALSE);
    *data = NULL;
    *len = 0;
    *name = NULL;
    if(prefer_text && has_plain_text())
        return FALSE;
    return from_gtk(data, len, name) || from_text(data, len, name);
}
