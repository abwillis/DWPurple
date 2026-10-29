/*
 * DWPurple - file transfer window and xfer UI ops.
 *
 * Every listed transfer holds a reference, so a row can never point to a
 * freed PurpleXfer; "Remove Finished" drops the references.
 */
#include "dwpurple.h"

typedef struct {
    HWND window, list;
    gboolean visible, refresh_pending;
    gint64 last_refresh;
} XferWin;

static XferWin xw;
static GList *xfers = NULL;

static char *size_text(goffset size)
{
    return g_format_size((guint64)(size < 0 ? 0 : size));
}

static const char *status_text(PurpleXfer *xfer)
{
    switch(purple_xfer_get_status(xfer))
    {
        case PURPLE_XFER_STATUS_NOT_STARTED:    return "Waiting";
        case PURPLE_XFER_STATUS_ACCEPTED:       return "Accepted";
        case PURPLE_XFER_STATUS_STARTED:        return "Transferring";
        case PURPLE_XFER_STATUS_DONE:           return "Finished";
        case PURPLE_XFER_STATUS_CANCEL_LOCAL:   return "Cancelled";
        case PURPLE_XFER_STATUS_CANCEL_REMOTE:  return "Cancelled by peer";
        default:                                return "Unknown";
    }
}

static void refresh(void)
{
    PurpleXfer *selected = NULL;
    GList *l;
    int count, row = 0;
    void *alloc;

    xw.refresh_pending = FALSE;
    xw.last_refresh = g_get_monotonic_time();
    if(!xw.window)
        return;
    selected = (PurpleXfer *)dw_container_query_start(xw.list, DW_CRA_SELECTED | DW_CR_RETDATA);
    dw_container_clear(xw.list, TRUE);
    count = (int)g_list_length(xfers);
    if(!count)
        return;
    alloc = dw_container_alloc(xw.list, count);
    for(l = xfers; l; l = l->next, row++)
    {
        PurpleXfer *xfer = l->data;
        char *file = (char *)(purple_xfer_get_filename(xfer) ? purple_xfer_get_filename(xfer) : "");
        char *peer = (char *)(purple_xfer_get_remote_user(xfer) ? purple_xfer_get_remote_user(xfer) : "");
        char *dir = purple_xfer_get_type(xfer) == PURPLE_XFER_SEND ? "Send" : "Receive";
        char *status = (char *)status_text(xfer);
        char *total = size_text((goffset)purple_xfer_get_size(xfer));
        char *done = size_text((goffset)purple_xfer_get_bytes_sent(xfer));
        char *progress = g_strdup_printf("%d%% (%s of %s)", (int)(purple_xfer_get_progress(xfer) * 100.0),
                                         done, total);

        dw_container_set_item(xw.list, alloc, 0, row, &file);
        dw_container_set_item(xw.list, alloc, 1, row, &peer);
        dw_container_set_item(xw.list, alloc, 2, row, &dir);
        dw_container_set_item(xw.list, alloc, 3, row, &status);
        dw_container_set_item(xw.list, alloc, 4, row, &progress);
        dw_container_set_row_title(alloc, row, file);
        dw_container_set_row_data(alloc, row, xfer);
        g_free(progress);
        g_free(done);
        g_free(total);
    }
    dw_container_insert(xw.list, alloc, count);
    if(selected && g_list_find(xfers, selected))
        dw_container_cursor_by_data(xw.list, selected);
}

static void refresh_task(gpointer unused)
{
    (void)unused;
    if(!xw.refresh_pending)
        return;
    /* Progress updates can be very frequent: at most ~3 refreshes/second. */
    if(g_get_monotonic_time() - xw.last_refresh < 300000)
    {
        dwp_defer(refresh_task, NULL);
        return;
    }
    refresh();
}

static void schedule_refresh(void)
{
    if(!xw.window || xw.refresh_pending)
        return;
    xw.refresh_pending = TRUE;
    dwp_defer(refresh_task, NULL);
}

static PurpleXfer *selected_xfer(void)
{
    PurpleXfer *xfer;

    if(!xw.window)
        return NULL;
    xfer = (PurpleXfer *)dw_container_query_start(xw.list, DW_CRA_SELECTED | DW_CR_RETDATA);
    return g_list_find(xfers, xfer) ? xfer : NULL;
}

static int DWSIGNAL xfer_cancel(HWND w, void *data)
{
    PurpleXfer *xfer = selected_xfer();

    (void)w;
    (void)data;
    if(xfer && !purple_xfer_is_completed(xfer) && !purple_xfer_is_canceled(xfer))
        purple_xfer_cancel_local(xfer);
    schedule_refresh();
    return TRUE;
}

static int DWSIGNAL xfer_copy_path(HWND w, void *data)
{
    PurpleXfer *xfer = selected_xfer();
    const char *path;

    (void)w;
    (void)data;
    if(xfer && (path = purple_xfer_get_local_filename(xfer)))
    {
        char *utf8 = dwp_filename_to_utf8(path);

        dw_clipboard_set_text(utf8, (int)strlen(utf8));
        g_free(utf8);
    }
    return TRUE;
}

static int DWSIGNAL xfer_clear(HWND w, void *data)
{
    GList *l, *next;

    (void)w;
    (void)data;
    /* Clear the rows first: unref may free the transfer. */
    if(xw.window)
        dw_container_clear(xw.list, TRUE);
    for(l = xfers; l; l = next)
    {
        PurpleXfer *xfer = l->data;

        next = l->next;
        if(purple_xfer_is_completed(xfer) || purple_xfer_is_canceled(xfer))
        {
            xfers = g_list_delete_link(xfers, l);
            purple_xfer_unref(xfer);
        }
    }
    refresh();
    return TRUE;
}

static int DWSIGNAL xfer_close(HWND w, void *data)
{
    (void)w;
    (void)data;
    if(xw.window)
    {
        dw_window_hide(xw.window);
        xw.visible = FALSE;
    }
    return TRUE;
}

static void ensure_window(void)
{
    unsigned long flags[5];
    char *titles[5] = { "File", "Peer", "Direction", "Status", "Progress" };
    HWND box, buttons;
    int z;

    if(xw.window)
        return;
    for(z = 0; z < 5; z++)
        flags[z] = DW_CFA_STRING | DW_CFA_LEFT | DW_CFA_HORZSEPARATOR | DW_CFA_SEPARATOR;
    xw.window = dwp_window_new("File Transfers", 0, 0);
    box = dw_box_new(DW_VERT, 4);
    dw_box_pack_start(xw.window, box, 0, 0, TRUE, TRUE, 0);
    xw.list = dw_container_new(0, FALSE);
    dw_container_setup(xw.list, flags, titles, 5, 0);
    dw_box_pack_start(box, xw.list, 600, 220, TRUE, TRUE, 2);
    buttons = dw_box_new(DW_HORZ, 0);
    dw_box_pack_start(box, buttons, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 2);
    dwp_button(buttons, "Cancel Transfer", DW_SIGNAL_FUNC(xfer_cancel), NULL);
    dwp_button(buttons, "Copy Path", DW_SIGNAL_FUNC(xfer_copy_path), NULL);
    dwp_button(buttons, "Remove Finished", DW_SIGNAL_FUNC(xfer_clear), NULL);
    dwp_spacer(buttons);
    dwp_button(buttons, "Close", DW_SIGNAL_FUNC(xfer_close), NULL);
    dw_signal_connect(xw.window, DW_SIGNAL_DELETE, DW_SIGNAL_FUNC(xfer_close), NULL);
}

void dwp_xfers_show(void)
{
    gboolean first = !xw.window;

    ensure_window();
    refresh();
    if(first)
        dwp_window_present(xw.window, 680, 320);
    else
    {
        dw_window_show(xw.window);
        dw_window_raise(xw.window);
    }
    xw.visible = TRUE;
}

/* ================================================================ ui ops */

static void ui_new_xfer(PurpleXfer *xfer)
{
    (void)xfer;
}

static void ui_destroy(PurpleXfer *xfer)
{
    /* Only reached for transfers we never listed (we hold a reference). */
    if(g_list_find(xfers, xfer))
    {
        if(xw.window)
            dw_container_clear(xw.list, TRUE);
        xfers = g_list_remove(xfers, xfer);
        schedule_refresh();
    }
}

static void ui_add_xfer(PurpleXfer *xfer)
{
    if(g_list_find(xfers, xfer))
        return;
    purple_xfer_ref(xfer);
    xfers = g_list_append(xfers, xfer);
    if(!xw.visible)
        dwp_xfers_show();
    else
        schedule_refresh();
}

static void ui_update_progress(PurpleXfer *xfer, double percent)
{
    (void)xfer;
    (void)percent;
    schedule_refresh();
}

static void ui_cancel(PurpleXfer *xfer)
{
    (void)xfer;
    schedule_refresh();
}

PurpleXferUiOps *dwp_xfer_ui_ops(void)
{
    static PurpleXferUiOps ops;
    static gboolean ready = FALSE;

    if(!ready)
    {
        memset(&ops, 0, sizeof(ops));
        ops.new_xfer = ui_new_xfer;
        ops.destroy = ui_destroy;
        ops.add_xfer = ui_add_xfer;
        ops.update_progress = ui_update_progress;
        ops.cancel_local = ui_cancel;
        ops.cancel_remote = ui_cancel;
        ready = TRUE;
    }
    return &ops;
}
