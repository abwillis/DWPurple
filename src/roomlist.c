/*
 * DWPurple - room list browser (PurpleRoomlistUiOps).
 *
 * The window holds a reference on the list it shows, so the room
 * pointers stored as row data stay valid until the rows are cleared.
 * Rooms arrive one at a time (thousands on IRC) and are appended to the
 * container in batches from the main loop.
 */
#include "dwpurple.h"

typedef struct {
    HWND window, list, status, stop, join, add, filter;
    DwpAccountCombo *account;
    PurpleRoomlist *roomlist;
    GPtrArray *pending;
    gboolean flush_pending;
    guint shown;
} RoomlistWin;

static RoomlistWin *rw = NULL;

static gboolean roomlist_capable(PurpleAccount *account)
{
    PurplePluginProtocolInfo *info = dwp_account_prpl_info(account, NULL);

    return info && info->roomlist_get_list;
}

static void set_status(void)
{
    char buf[128];

    if(!rw)
        return;
    if(!rw->roomlist)
        g_snprintf(buf, sizeof(buf), "Choose an account and press Get List.");
    else if(purple_roomlist_get_in_progress(rw->roomlist))
        g_snprintf(buf, sizeof(buf), "Downloading... %u rooms", rw->shown + (rw->pending ? rw->pending->len : 0));
    else
        g_snprintf(buf, sizeof(buf), "%u rooms", rw->shown);
    dw_window_set_text(rw->status, buf);
    if(rw->roomlist && purple_roomlist_get_in_progress(rw->roomlist))
        dw_window_enable(rw->stop);
    else
        dw_window_disable(rw->stop);
}

static char *room_details(PurpleRoomlist *list, PurpleRoomlistRoom *room)
{
    GString *s = g_string_new(NULL);
    GList *f = purple_roomlist_get_fields(list), *v = purple_roomlist_room_get_fields(room);

    for(; f && v; f = f->next, v = v->next)
    {
        PurpleRoomlistField *field = f->data;
        const char *label = purple_roomlist_field_get_label(field);

        if(purple_roomlist_field_get_hidden(field))
            continue;
        switch(purple_roomlist_field_get_type(field))
        {
            case PURPLE_ROOMLIST_FIELD_BOOL:
                if(GPOINTER_TO_INT(v->data))
                    g_string_append_printf(s, "%s%s", s->len ? "; " : "", label);
                break;
            case PURPLE_ROOMLIST_FIELD_INT:
                g_string_append_printf(s, "%s%s: %d", s->len ? "; " : "", label, GPOINTER_TO_INT(v->data));
                break;
            case PURPLE_ROOMLIST_FIELD_STRING:
                if(v->data && *(char *)v->data)
                {
                    char *plain = purple_markup_strip_html(v->data);
                    char *p;

                    for(p = plain; *p; p++)
                        if(*p == '\n' || *p == '\r' || *p == '\t')
                            *p = ' ';
                    g_string_append_printf(s, "%s%s: %s", s->len ? "; " : "", label, plain);
                    g_free(plain);
                }
                break;
        }
    }
    return g_string_free(s, FALSE);
}

static gboolean room_matches(PurpleRoomlistRoom *room, const char *filter, char *details)
{
    char *a, *b;
    gboolean ret;

    if(!filter || !*filter)
        return TRUE;
    a = g_utf8_casefold(purple_roomlist_room_get_name(room) ? purple_roomlist_room_get_name(room) : "", -1);
    b = g_utf8_casefold(details, -1);
    ret = strstr(a, filter) || strstr(b, filter);
    g_free(a);
    g_free(b);
    return ret;
}

static void append_rooms(GPtrArray *rooms)
{
    char *filter_raw = dwp_get_text_trimmed(rw->filter);
    char *filter = g_utf8_casefold(filter_raw, -1);
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *details = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *keep = g_ptr_array_new();
    void *alloc;
    guint z;

    for(z = 0; z < rooms->len; z++)
    {
        PurpleRoomlistRoom *room = g_ptr_array_index(rooms, z);
        PurpleRoomlistRoom *p;
        char *d = room_details(rw->roomlist, room);
        GString *name = g_string_new(NULL);

        if(!room_matches(room, filter, d))
        {
            g_free(d);
            g_string_free(name, TRUE);
            continue;
        }
        for(p = purple_roomlist_room_get_parent(room); p; p = purple_roomlist_room_get_parent(p))
            g_string_append(name, "    ");
        if(purple_roomlist_room_get_type(room) & PURPLE_ROOMLIST_ROOMTYPE_CATEGORY)
            g_string_append_printf(name, "[%s]", purple_roomlist_room_get_name(room));
        else
            g_string_append(name, purple_roomlist_room_get_name(room) ? purple_roomlist_room_get_name(room) : "");
        g_ptr_array_add(names, g_string_free(name, FALSE));
        g_ptr_array_add(details, d);
        g_ptr_array_add(keep, room);
    }
    if(keep->len)
    {
        alloc = dw_container_alloc(rw->list, (int)keep->len);
        for(z = 0; z < keep->len; z++)
        {
            char *n = g_ptr_array_index(names, z), *d = g_ptr_array_index(details, z);

            dw_container_set_item(rw->list, alloc, 0, (int)z, &n);
            dw_container_set_item(rw->list, alloc, 1, (int)z, &d);
            dw_container_set_row_title(alloc, (int)z, n);
            dw_container_set_row_data(alloc, (int)z, g_ptr_array_index(keep, z));
        }
        dw_container_insert(rw->list, alloc, (int)keep->len);
        rw->shown += keep->len;
    }
    g_ptr_array_free(keep, TRUE);
    g_ptr_array_free(details, TRUE);
    g_ptr_array_free(names, TRUE);
    g_free(filter);
    g_free(filter_raw);
}

static void flush_task(gpointer unused)
{
    GPtrArray *batch;

    (void)unused;
    if(!rw)
        return;
    rw->flush_pending = FALSE;
    if(!rw->roomlist || !rw->pending->len)
    {
        set_status();
        return;
    }
    batch = rw->pending;
    rw->pending = g_ptr_array_new();
    append_rooms(batch);
    g_ptr_array_free(batch, TRUE);
    set_status();
}

static void schedule_flush(void)
{
    if(!rw || rw->flush_pending)
        return;
    rw->flush_pending = TRUE;
    dwp_defer(flush_task, NULL);
}

static void release_list(void)
{
    PurpleRoomlist *list;

    if(!rw || !rw->roomlist)
        return;
    list = rw->roomlist;
    dw_container_clear(rw->list, TRUE);
    g_ptr_array_set_size(rw->pending, 0);
    rw->shown = 0;
    rw->roomlist = NULL;
    list->ui_data = NULL;
    if(purple_roomlist_get_in_progress(list))
        purple_roomlist_cancel_get_list(list);
    purple_roomlist_unref(list);
}

static void refilter(void)
{
    GPtrArray *all;
    GList *l;

    if(!rw || !rw->roomlist)
        return;
    dw_container_clear(rw->list, TRUE);
    rw->shown = 0;
    all = g_ptr_array_new();
    for(l = rw->roomlist->rooms; l; l = l->next)
        g_ptr_array_add(all, l->data);
    g_ptr_array_set_size(rw->pending, 0);
    append_rooms(all);
    g_ptr_array_free(all, TRUE);
    set_status();
}

static void fetch(PurpleAccount *account)
{
    PurpleConnection *gc = account ? purple_account_get_connection(account) : NULL;
    PurpleRoomlist *list;

    if(!rw)
        return;
    release_list();
    if(!gc)
    {
        dw_messagebox("Room List", DW_MB_OK | DW_MB_WARNING, "Please choose a connected account.");
        set_status();
        return;
    }
    list = purple_roomlist_get_list(gc);
    if(!list)
    {
        dw_messagebox("Room List", DW_MB_OK | DW_MB_WARNING, "This account could not provide a room list.");
        set_status();
        return;
    }
    purple_roomlist_ref(list);
    list->ui_data = rw;
    rw->roomlist = list;
    /* Rooms may already be present (synchronous protocols). */
    refilter();
}

static PurpleRoomlistRoom *selected_room(void)
{
    PurpleRoomlistRoom *room;
    GList *l;

    if(!rw || !rw->roomlist)
        return NULL;
    room = (PurpleRoomlistRoom *)dw_container_query_start(rw->list, DW_CRA_SELECTED | DW_CR_RETDATA);
    for(l = rw->roomlist->rooms; l; l = l->next)
        if(l->data == room)
            return room;
    return NULL;
}

static void activate_room(PurpleRoomlistRoom *room)
{
    if(!room || !rw->roomlist)
        return;
    if(purple_roomlist_room_get_type(room) & PURPLE_ROOMLIST_ROOMTYPE_ROOM)
        purple_roomlist_room_join(rw->roomlist, room);
    else
        purple_roomlist_expand_category(rw->roomlist, room);
}

static int DWSIGNAL rl_get(HWND w, void *data)
{
    (void)w; (void)data;
    fetch(dwp_account_combo_get(rw->account));
    return TRUE;
}

static int DWSIGNAL rl_stop(HWND w, void *data)
{
    (void)w; (void)data;
    if(rw->roomlist && purple_roomlist_get_in_progress(rw->roomlist))
        purple_roomlist_cancel_get_list(rw->roomlist);
    set_status();
    return TRUE;
}

static int DWSIGNAL rl_join(HWND w, void *data)
{
    (void)w; (void)data;
    activate_room(selected_room());
    return TRUE;
}

static int DWSIGNAL rl_enter(HWND w, char *text, void *data, void *itemdata)
{
    (void)w; (void)text; (void)data; (void)itemdata;
    activate_room(selected_room());
    return TRUE;
}

static int DWSIGNAL rl_add(HWND w, void *data)
{
    PurpleRoomlistRoom *room = selected_room();

    (void)w; (void)data;
    if(!room || !(purple_roomlist_room_get_type(room) & PURPLE_ROOMLIST_ROOMTYPE_ROOM))
        return TRUE;
    /* Pre-fill the Add Chat dialog via chat_info_defaults(name). */
    dwp_show_add_chat(rw->roomlist->account, NULL, NULL, purple_roomlist_room_get_name(room));
    return TRUE;
}

static int DWSIGNAL rl_filter(HWND w, void *data)
{
    (void)w; (void)data;
    refilter();
    return TRUE;
}

static void rw_free(gpointer p)
{
    RoomlistWin *d = p;

    g_ptr_array_free(d->pending, TRUE);
    dwp_account_combo_free(d->account);
    g_free(d);
}

static int DWSIGNAL rl_close(HWND w, void *data)
{
    RoomlistWin *d = rw;

    (void)w; (void)data;
    if(!d)
        return TRUE;
    release_list();
    rw = NULL;
    dwp_window_close_later(d->window, rw_free, d);
    return TRUE;
}

static void account_changed(gpointer data, PurpleAccount *account)
{
    (void)data;
    (void)account;
    release_list();
    set_status();
}

void dwp_roomlist_show(PurpleAccount *account, gboolean do_fetch)
{
    unsigned long flags[2] = {
        DW_CFA_STRING | DW_CFA_LEFT | DW_CFA_HORZSEPARATOR | DW_CFA_SEPARATOR,
        DW_CFA_STRING | DW_CFA_LEFT | DW_CFA_HORZSEPARATOR | DW_CFA_SEPARATOR
    };
    char *titles[2] = { "Room", "Details" };
    HWND box, row, buttons, apply;

    if(!rw)
    {
        rw = g_new0(RoomlistWin, 1);
        rw->pending = g_ptr_array_new();
        rw->window = dwp_window_new("Room List", 0, 0);
        box = dw_box_new(DW_VERT, 4);
        dw_box_pack_start(rw->window, box, 0, 0, TRUE, TRUE, 0);
        rw->account = dwp_account_combo_new(TRUE, roomlist_capable);
        row = dwp_form_row(box, "Account:", dwp_account_combo_widget(rw->account));
        dwp_button(row, "Get List", DW_SIGNAL_FUNC(rl_get), NULL);
        rw->filter = dw_entryfield_new("", 0);
        row = dwp_form_row(box, "Filter:", rw->filter);
        apply = dwp_button(row, "Apply", DW_SIGNAL_FUNC(rl_filter), NULL);
        dw_window_click_default(rw->filter, apply);
        rw->list = dw_container_new(0, FALSE);
        dw_container_setup(rw->list, flags, titles, 2, 0);
        dw_box_pack_start(box, rw->list, 560, 300, TRUE, TRUE, 2);
        dw_signal_connect(rw->list, DW_SIGNAL_ITEM_ENTER, DW_SIGNAL_FUNC(rl_enter), NULL);
        rw->status = dw_status_text_new("", 0);
        dw_box_pack_start(box, rw->status, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 1);
        buttons = dw_box_new(DW_HORZ, 0);
        dw_box_pack_start(box, buttons, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 2);
        rw->stop = dwp_button(buttons, "Stop", DW_SIGNAL_FUNC(rl_stop), NULL);
        rw->join = dwp_button(buttons, "Join / Expand", DW_SIGNAL_FUNC(rl_join), NULL);
        rw->add = dwp_button(buttons, "Add Chat...", DW_SIGNAL_FUNC(rl_add), NULL);
        dwp_spacer(buttons);
        dwp_button(buttons, "Close", DW_SIGNAL_FUNC(rl_close), NULL);
        dw_signal_connect(rw->window, DW_SIGNAL_DELETE, DW_SIGNAL_FUNC(rl_close), NULL);
        dwp_account_combo_on_change(rw->account, account_changed, NULL);
        dwp_window_present(rw->window, 640, 460);
    }
    else
    {
        dw_window_show(rw->window);
        dw_window_raise(rw->window);
    }
    if(account && roomlist_capable(account))
        dwp_account_combo_set(rw->account, account);
    set_status();
    if(do_fetch)
        fetch(dwp_account_combo_get(rw->account));
}

/* ================================================================ ui ops */

static void ui_show_with_account(PurpleAccount *account)
{
    dwp_roomlist_show(account, TRUE);
}

static void ui_create(PurpleRoomlist *list)
{
    (void)list;
}

static void ui_set_fields(PurpleRoomlist *list, GList *fields)
{
    (void)fields;
    if(rw && list == rw->roomlist)
        refilter();
}

static void ui_add_room(PurpleRoomlist *list, PurpleRoomlistRoom *room)
{
    if(!rw || list != rw->roomlist)
        return;
    g_ptr_array_add(rw->pending, room);
    schedule_flush();
}

static void ui_in_progress(PurpleRoomlist *list, gboolean flag)
{
    (void)flag;
    if(rw && list == rw->roomlist)
        schedule_flush();
}

static void ui_destroy(PurpleRoomlist *list)
{
    if(rw && list == rw->roomlist)
    {
        dw_container_clear(rw->list, TRUE);
        g_ptr_array_set_size(rw->pending, 0);
        rw->roomlist = NULL;
        rw->shown = 0;
        set_status();
    }
}

PurpleRoomlistUiOps *dwp_roomlist_ui_ops(void)
{
    static PurpleRoomlistUiOps ops;
    static gboolean ready = FALSE;

    if(!ready)
    {
        memset(&ops, 0, sizeof(ops));
        ops.show_with_account = ui_show_with_account;
        ops.create = ui_create;
        ops.set_fields = ui_set_fields;
        ops.add_room = ui_add_room;
        ops.in_progress = ui_in_progress;
        ops.destroy = ui_destroy;
        ready = TRUE;
    }
    return &ops;
}
