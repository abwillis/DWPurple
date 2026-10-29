/*
 * DWPurple - debug window (libpurple debug UI ops).
 */
#include "dwpurple.h"

#define MAX_DEBUG_CHARS 120000

typedef struct {
    HWND window, mle, level, pause;
    gboolean visible;
    unsigned int pos;
    unsigned long chars;
} DebugWin;

static DebugWin dbg;
static GThread *main_thread = NULL;
static char *level_labels[] = { "All", "Misc", "Info", "Warning", "Error", "Fatal" };

static const char *level_name(PurpleDebugLevel level)
{
    switch(level)
    {
        case PURPLE_DEBUG_MISC:    return "misc";
        case PURPLE_DEBUG_INFO:    return "info";
        case PURPLE_DEBUG_WARNING: return "warning";
        case PURPLE_DEBUG_ERROR:   return "error";
        case PURPLE_DEBUG_FATAL:   return "fatal";
        default:                   return "all";
    }
}

static PurpleDebugLevel min_level(void)
{
    int index = dwp_combo_index(dbg.level, level_labels, 6);

    return index > 0 ? (PurpleDebugLevel)index : PURPLE_DEBUG_ALL;
}

static void debug_print(PurpleDebugLevel level, const char *category, const char *arg_s)
{
    char ts[32], *line, **parts, *crlf;
    time_t now;
    struct tm *tm;

    /* Widgets may only be touched from the GUI thread. */
    if(!dbg.window || !dbg.visible || g_thread_self() != main_thread)
        return;
    if(dw_checkbox_get(dbg.pause) || level < min_level())
        return;
    now = time(NULL);
    tm = localtime(&now);
    if(!tm || !strftime(ts, sizeof(ts), "%H:%M:%S", tm))
        strcpy(ts, "--:--:--");
    line = g_strdup_printf("(%s) %s%s%s: %s", ts, category ? category : "",
                           category ? " " : "", level_name(level), arg_s ? arg_s : "");
    if(!g_utf8_validate(line, -1, NULL))
    {
        char *fixed = purple_utf8_salvage(line);

        g_free(line);
        line = fixed;
    }
    g_strchomp(line);
    parts = g_strsplit(line, "\n", -1);
    crlf = g_strjoinv("\r\n", parts);
    g_strfreev(parts);
    g_free(line);
    if(dbg.chars > MAX_DEBUG_CHARS)
    {
        dw_mle_clear(dbg.mle);
        dbg.pos = 0;
        dbg.chars = 0;
    }
    dbg.pos = dw_mle_import(dbg.mle, crlf, dbg.pos);
    dbg.pos = dw_mle_import(dbg.mle, "\r\n", dbg.pos);
    dbg.chars += strlen(crlf) + 2;
    dw_mle_set_cursor(dbg.mle, dbg.pos);
    g_free(crlf);
}

static gboolean debug_is_enabled(PurpleDebugLevel level, const char *category)
{
    (void)level;
    (void)category;
    return dbg.window && dbg.visible;
}

PurpleDebugUiOps *dwp_debug_ui_ops(void)
{
    static PurpleDebugUiOps ops;
    static gboolean ready = FALSE;

    if(!ready)
    {
        main_thread = g_thread_self();
        memset(&ops, 0, sizeof(ops));
        ops.print = debug_print;
        ops.is_enabled = debug_is_enabled;
        ready = TRUE;
    }
    return &ops;
}

static int DWSIGNAL debug_clear(HWND w, void *data)
{
    (void)w;
    (void)data;
    dw_mle_clear(dbg.mle);
    dbg.pos = 0;
    dbg.chars = 0;
    return TRUE;
}

static int DWSIGNAL debug_copy(HWND w, void *data)
{
    char *text = dwp_mle_get_text(dbg.mle);

    (void)w;
    (void)data;
    dw_clipboard_set_text(text, (int)strlen(text));
    g_free(text);
    return TRUE;
}

static int DWSIGNAL debug_close(HWND w, void *data)
{
    (void)w;
    (void)data;
    dw_window_hide(dbg.window);
    dbg.visible = FALSE;
    return TRUE;
}

void dwp_debug_show(void)
{
    HWND box, row;

    if(!dbg.window)
    {
        dbg.window = dwp_window_new("Debug Window", 0, 0);
        box = dw_box_new(DW_VERT, 2);
        dw_box_pack_start(dbg.window, box, 0, 0, TRUE, TRUE, 0);
        row = dw_box_new(DW_HORZ, 0);
        dw_box_pack_start(box, row, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 2);
        dw_box_pack_start(row, dwp_label("Level:"), DW_SIZE_AUTO, DW_SIZE_AUTO, FALSE, TRUE, 2);
        dbg.level = dw_combobox_new("", 0);
        dwp_combo_fill(dbg.level, level_labels, 6, 0);
        dw_box_pack_start(row, dbg.level, 120, DW_SIZE_AUTO, FALSE, FALSE, 2);
        dbg.pause = dw_checkbox_new("Pause", 0);
        dw_box_pack_start(row, dbg.pause, DW_SIZE_AUTO, DW_SIZE_AUTO, FALSE, FALSE, 2);
        dwp_spacer(row);
        dwp_button(row, "Copy", DW_SIGNAL_FUNC(debug_copy), NULL);
        dwp_button(row, "Clear", DW_SIGNAL_FUNC(debug_clear), NULL);
        dwp_button(row, "Close", DW_SIGNAL_FUNC(debug_close), NULL);
        dbg.mle = dw_mle_new(0);
        dw_mle_set_editable(dbg.mle, FALSE);
        dw_mle_set_word_wrap(dbg.mle, TRUE);
        dw_box_pack_start(box, dbg.mle, 600, 360, TRUE, TRUE, 2);
        dw_signal_connect(dbg.window, DW_SIGNAL_DELETE, DW_SIGNAL_FUNC(debug_close), NULL);
        dwp_window_present(dbg.window, 720, 460);
    }
    else
    {
        dw_window_show(dbg.window);
        dw_window_raise(dbg.window);
    }
    dbg.visible = TRUE;
}
