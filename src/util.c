/*
 * DWPurple - shared helpers: strings, deferred tasks, menus, layout,
 * prompts and account/group combo boxes.
 */
#include "dwpurple.h"

#include <ctype.h>
#include <stdarg.h>

/* ================================================================ strings */

char *dwp_get_text(HWND handle)
{
    char *tmp, *ret;

    if(!handle)
        return g_strdup("");
    tmp = dw_window_get_text(handle);
    ret = g_strdup(tmp ? tmp : "");
    if(tmp)
        dw_free(tmp);
    return ret;
}

char *dwp_get_text_trimmed(HWND handle)
{
    return g_strstrip(dwp_get_text(handle));
}

static void strip_cr(char *s)
{
    char *in, *out;

    for(in = out = s; *in; in++)
        if(*in != '\r')
            *out++ = *in;
    *out = '\0';
}

char *dwp_mle_get_text(HWND mle)
{
    unsigned long size = 0;
    char *tmp, *buf, *ret;

    if(!mle)
        return g_strdup("");
    /* dw_window_get_text() handles MLEs on Windows and OS/2 (full UTF-8);
     * the GTK ports only support it for entry fields, so fall back to
     * dw_mle_export() there.  GTK counts characters, so size the buffer for
     * multi-byte UTF-8. */
    tmp = dw_window_get_text(mle);
    if(tmp && *tmp)
    {
        ret = g_strdup(tmp);
        dw_free(tmp);
        strip_cr(ret);
        return ret;
    }
    if(tmp)
        dw_free(tmp);
    dw_mle_get_size(mle, &size, NULL);
    if(!size)
        return g_strdup("");
    buf = g_malloc0(size * 6 + 16);
    dw_mle_export(mle, buf, 0, (int)size);
    ret = g_strdup(buf);
    g_free(buf);
    strip_cr(ret);
    if(!g_utf8_validate(ret, -1, NULL))
    {
        char *fixed = purple_utf8_salvage(ret);

        g_free(ret);
        ret = fixed;
    }
    return ret;
}

unsigned int dwp_mle_set_text(HWND mle, const char *text)
{
    dw_mle_clear(mle);
    if(!text || !*text)
        return 0;
    return dw_mle_import(mle, text, 0);
}

gboolean dwp_account_exists(PurpleAccount *account)
{
    return account && g_list_find(purple_accounts_get_all(), account) != NULL;
}

gboolean dwp_connection_exists(PurpleConnection *gc)
{
    return gc && g_list_find(purple_connections_get_all(), gc) != NULL;
}

gboolean dwp_conversation_exists(PurpleConversation *conv)
{
    return conv && g_list_find(purple_get_conversations(), conv) != NULL;
}

gboolean dwp_blist_node_exists(PurpleBlistNode *node)
{
    PurpleBlistNode *n;

    if(!node)
        return FALSE;
    for(n = purple_blist_get_root(); n; n = purple_blist_node_next(n, TRUE))
        if(n == node)
            return TRUE;
    return FALSE;
}

PurplePluginProtocolInfo *dwp_prpl_info(PurpleConnection *gc)
{
    PurplePlugin *prpl;

    if(!gc)
        return NULL;
    prpl = purple_connection_get_prpl(gc);
    return prpl ? PURPLE_PLUGIN_PROTOCOL_INFO(prpl) : NULL;
}

PurplePluginProtocolInfo *dwp_account_prpl_info(PurpleAccount *account, PurpleConnection **gc_out)
{
    PurpleConnection *gc = account ? purple_account_get_connection(account) : NULL;

    if(gc_out)
        *gc_out = gc;
    return dwp_prpl_info(gc);
}

char *dwp_account_display_name(PurpleAccount *account)
{
    if(!account)
        return g_strdup("");
    return g_strdup_printf("%s (%s)", purple_account_get_username(account),
                           purple_account_get_protocol_name(account));
}

const char *dwp_primitive_label(PurpleStatusPrimitive prim)
{
    switch(prim)
    {
        case PURPLE_STATUS_AVAILABLE:      return "Available";
        case PURPLE_STATUS_AWAY:           return "Away";
        case PURPLE_STATUS_UNAVAILABLE:    return "Do Not Disturb";
        case PURPLE_STATUS_EXTENDED_AWAY:  return "Extended Away";
        case PURPLE_STATUS_INVISIBLE:      return "Invisible";
        case PURPLE_STATUS_OFFLINE:        return "Offline";
        default:                           return purple_primitive_get_name_from_type(prim);
    }
}

PurpleStatusPrimitive dwp_account_primitive(PurpleAccount *account)
{
    PurpleStatus *status;

    if(!account || !purple_account_is_connected(account))
        return PURPLE_STATUS_OFFLINE;
    status = purple_account_get_active_status(account);
    return status ? purple_status_type_get_primitive(purple_status_get_type(status))
                  : PURPLE_STATUS_AVAILABLE;
}

PurpleStatusPrimitive dwp_buddy_primitive(PurpleBuddy *buddy, gboolean *idle)
{
    PurplePresence *presence;
    PurpleStatus *status;

    if(idle)
        *idle = FALSE;
    if(!buddy || !PURPLE_BUDDY_IS_ONLINE(buddy))
        return PURPLE_STATUS_OFFLINE;
    presence = purple_buddy_get_presence(buddy);
    if(idle)
        *idle = purple_presence_is_idle(presence);
    status = purple_presence_get_active_status(presence);
    return status ? purple_status_type_get_primitive(purple_status_get_type(status))
                  : PURPLE_STATUS_AVAILABLE;
}

char *dwp_mnemonic(const char *text)
{
    GString *out = g_string_new(NULL);
    const char *p;

    for(p = text ? text : ""; *p; p++)
    {
        if(*p == '_')
        {
            if(p[1] == '_')
            {
                g_string_append_c(out, '_');
                p++;
            }
            else
                g_string_append_c(out, '~');
        }
        else if(*p == '~')
            g_string_append_c(out, '-');
        else
            g_string_append_c(out, *p);
    }
    return g_string_free(out, FALSE);
}

char *dwp_strip_mnemonic(const char *text)
{
    GString *out = g_string_new(NULL);
    const char *p;

    for(p = text ? text : ""; *p; p++)
    {
        if(*p == '_')
        {
            if(p[1] == '_')
            {
                g_string_append_c(out, '_');
                p++;
            }
        }
        else
            g_string_append_c(out, *p);
    }
    return g_string_free(out, FALSE);
}

gboolean dwp_open_link(const char *url)
{
    static const char *const allowed[] = { "http:", "https:", "mailto:", "xmpp:", "irc:", "ircs:", NULL };
    int z;

    if(!url || !*url)
        return FALSE;
    for(z = 0; allowed[z]; z++)
    {
        if(g_ascii_strncasecmp(url, allowed[z], strlen(allowed[z])) == 0)
        {
            dw_browse(url);
            return TRUE;
        }
    }
    purple_debug_warning(DWP_UI_ID, "Refusing to open link with unsupported scheme\n");
    return FALSE;
}

char *dwp_filename_to_utf8(const char *name)
{
    char *ret;

    if(!name)
        return g_strdup("");
    ret = g_filename_to_utf8(name, -1, NULL, NULL, NULL);
    return ret ? ret : purple_utf8_salvage(name);
}

/* ================================================================== icons */

static HICN status_icons[8];
static gboolean icons_loaded = FALSE;
static HICN chat_icon_h, app_icon_h;

static HICN load_icon_from(const char *dir, const char *name)
{
    char *path = g_build_filename(dir, name, NULL);
    HICN icon = 0;

#ifndef _WIN32
    /* OS/2 tries "<name>.ico" first and only understands OS/2 icons there,
     * so ask for the PNG explicitly; GTK and macOS load PNG anyway. */
    {
        char *png = g_strdup_printf("%s.png", path);

        if(g_file_test(png, G_FILE_TEST_IS_REGULAR))
            icon = dw_icon_load_from_file(png);
        g_free(png);
    }
#endif
    /* Windows: .ico first; elsewhere whatever the port finds. */
    if(!icon)
        icon = dw_icon_load_from_file(path);
    g_free(path);
    return icon;
}

static char *exe_dir = NULL;

void dwp_set_exe_path(const char *argv0)
{
    char *path = NULL;

#ifdef __linux__
    path = g_file_read_link("/proc/self/exe", NULL);
#endif
    if(!path && argv0 && *argv0)
        path = g_strdup(argv0);
    g_free(exe_dir);
    exe_dir = path ? g_path_get_dirname(path) : NULL;
    g_free(path);
}

static HICN load_icon(const char *name)
{
    char *dir;
    HICN icon = 0;

    /* <exe dir>/icons (build tree, OS/2 and Windows installs),
     * <dw app dir>/icons (<prefix>/share/dwpurple/icons), <config>/ui-icons. */
    if(exe_dir)
    {
        dir = g_build_filename(exe_dir, "icons", NULL);
        icon = load_icon_from(dir, name);
        g_free(dir);
    }
    if(!icon)
    {
        dir = g_build_filename(dw_app_dir(), "icons", NULL);
        icon = load_icon_from(dir, name);
        g_free(dir);
    }
    if(!icon)
    {
        dir = g_build_filename(purple_user_dir(), "ui-icons", NULL);
        icon = load_icon_from(dir, name);
        g_free(dir);
    }
    return icon;
}

static void icons_load(void)
{
    static const char *const names[8] = {
        "offline", "available", "away", "busy", "xa", "invisible", "idle", "unknown"
    };
    int z;

    if(icons_loaded)
        return;
    icons_loaded = TRUE;
    for(z = 0; z < 8; z++)
        status_icons[z] = load_icon(names[z]);
    chat_icon_h = load_icon("chat");
    app_icon_h = load_icon("dwpurple");
    if(!app_icon_h)
        app_icon_h = status_icons[1];
}

HICN dwp_status_icon(PurpleStatusPrimitive prim, gboolean idle)
{
    icons_load();
    if(idle && prim != PURPLE_STATUS_OFFLINE && status_icons[6])
        return status_icons[6];
    switch(prim)
    {
        case PURPLE_STATUS_OFFLINE:       return status_icons[0];
        case PURPLE_STATUS_AVAILABLE:
        case PURPLE_STATUS_TUNE:
        case PURPLE_STATUS_MOOD:          return status_icons[1];
        case PURPLE_STATUS_AWAY:          return status_icons[2];
        case PURPLE_STATUS_UNAVAILABLE:   return status_icons[3];
        case PURPLE_STATUS_EXTENDED_AWAY: return status_icons[4];
        case PURPLE_STATUS_INVISIBLE:     return status_icons[5];
        default:                          return status_icons[7];
    }
}

HICN dwp_chat_icon(void)
{
    icons_load();
    return chat_icon_h;
}

HICN dwp_app_icon(void)
{
    icons_load();
    return app_icon_h;
}

void dwp_icons_free(void)
{
    /* Icons are released by the windowing system at exit; nothing to do
     * portably (dw_icon_free on a shared handle would break live widgets). */
}

static time_t last_activity = 0;

void dwp_note_activity(void)
{
    last_activity = time(NULL);
}

time_t dwp_last_activity(void)
{
    if(!last_activity)
        last_activity = time(NULL);
    return last_activity;
}

/* ========================================================= deferred tasks */

typedef struct {
    DwpTaskFunc func;
    gpointer data;
} DwpTask;

static GQueue task_queue = G_QUEUE_INIT;
static HTIMER task_timer = 0;

static int DWSIGNAL task_timer_cb(void *unused)
{
    int budget = 64;

    (void)unused;
    dwp_gui_lock();
    /* Pop one task at a time: a task may start a nested main loop
     * (modal dialog) during which this timer can fire again. */
    while(budget-- > 0 && !g_queue_is_empty(&task_queue))
    {
        DwpTask *task = g_queue_pop_head(&task_queue);
        DwpTaskFunc func = task->func;
        gpointer data = task->data;

        g_free(task);
        func(data);
    }
    dwp_gui_unlock();
    return TRUE;
}

void dwp_defer(DwpTaskFunc func, gpointer data)
{
    DwpTask *task;

    g_return_if_fail(func != NULL);
    task = g_new0(DwpTask, 1);
    task->func = func;
    task->data = data;
    g_queue_push_tail(&task_queue, task);
}

void dwp_tasks_start(void)
{
    if(!task_timer)
        task_timer = dw_timer_connect(30, DW_SIGNAL_FUNC(task_timer_cb), NULL);
}

void dwp_tasks_stop(void)
{
    /* Run what is left so windows are destroyed and memory released. */
    task_timer_cb(NULL);
    if(task_timer)
    {
        dw_timer_disconnect(task_timer);
        task_timer = 0;
    }
}

typedef struct {
    HWND window;
    GDestroyNotify free_data;
    gpointer data;
} CloseLater;

static void close_later_task(gpointer p)
{
    CloseLater *cl = p;

    if(cl->window)
        dw_window_destroy(cl->window);
    if(cl->free_data)
        cl->free_data(cl->data);
    g_free(cl);
}

void dwp_window_close_later(HWND window, GDestroyNotify free_data, gpointer data)
{
    CloseLater *cl = g_new0(CloseLater, 1);

    if(window)
        dw_window_hide(window);
    cl->window = window;
    cl->free_data = free_data;
    cl->data = data;
    dwp_defer(close_later_task, cl);
}

/* ============================================================ menu actions */

typedef struct {
    DwpActionFunc func;
    gpointer a, b;
    char *str;
} DwpAction;

static GPtrArray *popup_actions = NULL;
static int menu_syncing = 0;

static void action_free(gpointer p)
{
    DwpAction *act = p;

    g_free(act->str);
    g_free(act);
}

void dwp_actions_reset(void)
{
    if(popup_actions)
        g_ptr_array_set_size(popup_actions, 0);
}

static int DWSIGNAL action_clicked(HWND window, void *data)
{
    DwpAction *act = data;
    DwpActionFunc func;
    gpointer a, b;
    char *str;

    (void)window;
    /* State changes made by dwp_menu_set_state() are not user clicks. */
    if(menu_syncing || !act || !act->func)
        return FALSE;
    /* Copy first: the action may rebuild menus and release this record. */
    func = act->func;
    a = act->a;
    b = act->b;
    str = g_strdup(act->str);
    dwp_note_activity();
    func(a, b, str);
    g_free(str);
    return FALSE;
}

HWND dwp_menu_add(HMENUI menu, const char *title, DwpActionFunc func,
                  gpointer a, gpointer b, const char *str)
{
    HWND item;
    DwpAction *act;

    item = dw_menu_append_item(menu, title, DW_MENU_POPUP, 0, TRUE, FALSE, DW_NOMENU);
    if(!item || !func)
        return item;
    if(!popup_actions)
        popup_actions = g_ptr_array_new_with_free_func(action_free);
    act = g_new0(DwpAction, 1);
    act->func = func;
    act->a = a;
    act->b = b;
    act->str = g_strdup(str);
    g_ptr_array_add(popup_actions, act);
    dw_signal_connect(item, DW_SIGNAL_CLICKED, DW_SIGNAL_FUNC(action_clicked), act);
    return item;
}

HWND dwp_menu_add_static(HMENUI menu, const char *title, unsigned long id, int check,
                         DwpActionFunc func, gpointer a)
{
    HWND item = dw_menu_append_item(menu, title, id ? id : DW_MENU_AUTO, 0, TRUE, check, DW_NOMENU);

    if(item && func)
    {
        DwpAction *act = g_new0(DwpAction, 1);

        act->func = func;
        act->a = a;
        /* Lives for the life of the program. */
        dw_signal_connect(item, DW_SIGNAL_CLICKED, DW_SIGNAL_FUNC(action_clicked), act);
    }
    return item;
}

void dwp_menu_separator(HMENUI menu)
{
    dw_menu_append_item(menu, DW_MENU_SEPARATOR, DW_MENU_POPUP, 0, TRUE, FALSE, DW_NOMENU);
}

void dwp_menu_set_state(HMENUI menu, unsigned long id, unsigned long state)
{
    if(!menu)
        return;
    menu_syncing++;
    dw_menu_item_set_state(menu, id, state);
    menu_syncing--;
}

void dwp_menu_popup_here(HMENUI *menu, HWND parent)
{
    long x = 0, y = 0;

    dw_pointer_query_pos(&x, &y);
    dw_menu_popup(menu, parent, (int)x, (int)y);
}

/* ================================================================== layout */

HWND dwp_window_new(const char *title, int width, int height)
{
    HWND win = dw_window_new(HWND_DESKTOP, title,
                             DW_FCF_SYSMENU | DW_FCF_TITLEBAR | DW_FCF_SIZEBORDER |
                             DW_FCF_MINMAX | DW_FCF_TASKLIST);
    HICN icon = dwp_app_icon();

    if(icon)
        dw_window_set_icon(win, icon);
    if(width > 0 && height > 0)
        dw_window_set_size(win, width, height);
    return win;
}

HWND dwp_label(const char *text)
{
    HWND label = dw_text_new(text ? text : "", 0);

    dw_window_set_style(label, DW_DT_VCENTER, DW_DT_VCENTER);
    return label;
}

HWND dwp_form_row(HWND box, const char *label, HWND widget)
{
    HWND row = dw_box_new(DW_HORZ, 0);

    dw_box_pack_start(row, dwp_label(label), 150, DW_SIZE_AUTO, FALSE, TRUE, 2);
    if(widget)
        dw_box_pack_start(row, widget, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, TRUE, 2);
    dw_box_pack_start(box, row, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 1);
    return row;
}

HWND dwp_button(HWND box, const char *text, void *func, void *data)
{
    HWND button = dw_button_new(text, 0);

    dw_box_pack_start(box, button, DW_SIZE_AUTO, DW_SIZE_AUTO, FALSE, FALSE, 2);
    if(func)
        dw_signal_connect(button, DW_SIGNAL_CLICKED, func, data);
    return button;
}

HWND dwp_text_block(HWND box, const char *text, int width, int max_height)
{
    char *copy = g_strdup(text ? text : ""), *crlf, **parts;
    int z, lines = 0, cols, height;
    HWND mle;

    /* Normalise to \n, then to the \r\n the platform edit controls expect. */
    strip_cr(copy);
    parts = g_strsplit(copy, "\n", -1);
    g_free(copy);
    /* Rough wrapped line count: ~7 pixels per character. */
    cols = width > 90 ? (width - 20) / 7 : 10;
    for(z = 0; parts[z]; z++)
        lines += 1 + (int)(g_utf8_strlen(parts[z], -1) / cols);
    crlf = g_strjoinv("\r\n", parts);
    g_strfreev(parts);
    height = lines * 18 + 14;
    if(height < 34)
        height = 34;
    if(max_height > 0 && height > max_height)
        height = max_height;
    mle = dw_mle_new(0);
    dw_mle_set_editable(mle, FALSE);
    dw_mle_set_word_wrap(mle, TRUE);
    dw_mle_import(mle, crlf, 0);
    dw_mle_set_cursor(mle, 0);
    dw_box_pack_start(box, mle, width, height, TRUE, TRUE, 3);
    g_free(crlf);
    return mle;
}

HWND dwp_box_reset(HWND holder, HWND old, int type)
{
    HWND box;

    if(old)
    {
        dw_box_unpack(old);
        dw_window_destroy(old);
    }
    box = dw_box_new(type, 0);
    dw_box_pack_start(holder, box, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, TRUE, 0);
    return box;
}

void dwp_spacer(HWND box)
{
    dw_box_pack_start(box, 0, 1, 1, TRUE, FALSE, 0);
}

void dwp_window_present(HWND window, int width, int height)
{
    int sw = dw_screen_width(), sh = dw_screen_height();

    if(width > 0 && height > 0)
    {
        if(width > sw)
            width = sw;
        if(height > sh)
            height = sh;
        dw_window_set_pos_size(window, (sw - width) / 2, (sh - height) / 2, width, height);
    }
    dw_window_show(window);
    dw_window_raise(window);
}

int dwp_combo_index(HWND combo, char **items, int count)
{
    int index = dw_listbox_selected(combo);
    char *text;
    int z;

    text = dwp_get_text(combo);
    /* An edited combo entry leaves a stale index; trust the text first. */
    if(index >= 0 && index < count && items && purple_strequal(items[index], text))
    {
        g_free(text);
        return index;
    }
    for(z = 0; z < count && items; z++)
    {
        if(purple_strequal(items[z], text))
        {
            g_free(text);
            return z;
        }
    }
    g_free(text);
    return (index >= 0 && index < count) ? index : -1;
}

void dwp_combo_fill(HWND combo, char **items, int count, int selected)
{
    int z;

    dw_listbox_clear(combo);
    for(z = 0; z < count; z++)
        dw_listbox_append(combo, items[z]);
    if(selected >= 0 && selected < count)
    {
        dw_listbox_select(combo, selected, TRUE);
        dw_window_set_text(combo, items[selected]);
    }
    else
        dw_window_set_text(combo, "");
}

/* ================================================================ prompts */

typedef struct {
    HWND window, entry;
    DwpPromptFunc ok;
    gpointer data;
} Prompt;

static int DWSIGNAL prompt_ok(HWND button, void *data)
{
    Prompt *p = data;
    char *text = dwp_get_text(p->entry);
    DwpPromptFunc ok = p->ok;
    gpointer udata = p->data;

    (void)button;
    p->ok = NULL;
    dwp_window_close_later(p->window, g_free, p);
    if(ok)
        ok(udata, text);
    g_free(text);
    return FALSE;
}

static int DWSIGNAL prompt_cancel(HWND button, void *data)
{
    Prompt *p = data;

    (void)button;
    p->ok = NULL;
    dwp_window_close_later(p->window, g_free, p);
    return TRUE;
}

void dwp_prompt(const char *title, const char *label, const char *initial, gboolean masked,
                DwpPromptFunc ok, gpointer data)
{
    Prompt *p = g_new0(Prompt, 1);
    HWND box, buttons, okb;

    p->ok = ok;
    p->data = data;
    p->window = dwp_window_new(title, 0, 0);
    box = dw_box_new(DW_VERT, 4);
    dw_box_pack_start(p->window, box, 0, 0, TRUE, TRUE, 0);
    dw_box_pack_start(box, dwp_label(label), DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 2);
    p->entry = masked ? dw_entryfield_password_new(initial ? initial : "", 0)
                      : dw_entryfield_new(initial ? initial : "", 0);
    dw_box_pack_start(box, p->entry, 300, DW_SIZE_AUTO, TRUE, FALSE, 2);
    buttons = dw_box_new(DW_HORZ, 0);
    dw_box_pack_start(box, buttons, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 2);
    dwp_spacer(buttons);
    okb = dwp_button(buttons, "OK", DW_SIGNAL_FUNC(prompt_ok), p);
    dwp_button(buttons, "Cancel", DW_SIGNAL_FUNC(prompt_cancel), p);
    dw_signal_connect(p->window, DW_SIGNAL_DELETE, DW_SIGNAL_FUNC(prompt_cancel), p);
    dw_window_click_default(p->entry, okb);
    dwp_window_present(p->window, 380, 130);
    dw_window_set_focus(p->entry);
}

typedef struct {
    HWND window;
} MessageWin;

static int DWSIGNAL message_close(HWND button, void *data)
{
    MessageWin *m = data;

    (void)button;
    dwp_window_close_later(m->window, g_free, m);
    return TRUE;
}

void dwp_message(const char *title, const char *primary, const char *secondary)
{
    MessageWin *m = g_new0(MessageWin, 1);
    HWND box, buttons, okb;

    m->window = dwp_window_new(title ? title : DWP_NAME, 0, 0);
    box = dw_box_new(DW_VERT, 4);
    dw_box_pack_start(m->window, box, 0, 0, TRUE, TRUE, 0);
    if(primary && *primary)
        dwp_text_block(box, primary, 380, 120);
    if(secondary && *secondary)
        dwp_text_block(box, secondary, 380, 200);
    buttons = dw_box_new(DW_HORZ, 0);
    dw_box_pack_start(box, buttons, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 2);
    dwp_spacer(buttons);
    okb = dwp_button(buttons, "Close", DW_SIGNAL_FUNC(message_close), m);
    dw_signal_connect(m->window, DW_SIGNAL_DELETE, DW_SIGNAL_FUNC(message_close), m);
    dwp_window_present(m->window, 420, secondary && *secondary ? 260 : 140);
    dw_window_set_focus(okb);
}

gboolean dwp_confirm(const char *title, const char *fmt, ...)
{
    va_list args;
    char *text;
    int rc;

    va_start(args, fmt);
    text = g_strdup_vprintf(fmt, args);
    va_end(args);
    rc = dw_messagebox(title, DW_MB_YESNO | DW_MB_QUESTION, "%s", text);
    g_free(text);
    return rc == DW_MB_RETURN_YES;
}

/* ========================================================= account combo */

struct _DwpAccountCombo {
    HWND combo;
    GPtrArray *accounts;
    GPtrArray *labels;
    gboolean online_only;
    DwpAccountFilter filter;
    DwpAccountChanged changed;
    gpointer changed_data;
    gboolean filling;
    int handle;      /* address used as the purple signal handle */
};

static int DWSIGNAL account_combo_select(HWND combo, int index, void *data)
{
    DwpAccountCombo *ac = data;

    (void)combo;
    (void)index;
    if(!ac->filling && ac->changed)
        ac->changed(ac->changed_data, dwp_account_combo_get(ac));
    return FALSE;
}

static void account_combo_signal(PurpleAccount *account, gpointer data)
{
    (void)account;
    dwp_account_combo_refill(data);
}

DwpAccountCombo *dwp_account_combo_new(gboolean online_only, DwpAccountFilter filter)
{
    DwpAccountCombo *ac = g_new0(DwpAccountCombo, 1);
    void *handle = purple_accounts_get_handle();

    ac->combo = dw_combobox_new("", 0);
    ac->accounts = g_ptr_array_new();
    ac->labels = g_ptr_array_new_with_free_func(g_free);
    ac->online_only = online_only;
    ac->filter = filter;
    dw_signal_connect(ac->combo, DW_SIGNAL_LIST_SELECT, DW_SIGNAL_FUNC(account_combo_select), ac);
    purple_signal_connect(handle, "account-signed-on", &ac->handle, PURPLE_CALLBACK(account_combo_signal), ac);
    purple_signal_connect(handle, "account-signed-off", &ac->handle, PURPLE_CALLBACK(account_combo_signal), ac);
    purple_signal_connect(handle, "account-added", &ac->handle, PURPLE_CALLBACK(account_combo_signal), ac);
    purple_signal_connect(handle, "account-removed", &ac->handle, PURPLE_CALLBACK(account_combo_signal), ac);
    purple_signal_connect(handle, "account-enabled", &ac->handle, PURPLE_CALLBACK(account_combo_signal), ac);
    purple_signal_connect(handle, "account-disabled", &ac->handle, PURPLE_CALLBACK(account_combo_signal), ac);
    dwp_account_combo_refill(ac);
    return ac;
}

HWND dwp_account_combo_widget(DwpAccountCombo *ac)
{
    return ac->combo;
}

void dwp_account_combo_refill(DwpAccountCombo *ac)
{
    PurpleAccount *current = dwp_account_combo_get(ac);
    GList *l;
    int z, sel = -1;

    ac->filling = TRUE;
    g_ptr_array_set_size(ac->accounts, 0);
    g_ptr_array_set_size(ac->labels, 0);
    for(l = purple_accounts_get_all(); l; l = l->next)
    {
        PurpleAccount *account = l->data;

        if(ac->online_only && !purple_account_is_connected(account))
            continue;
        if(ac->filter && !ac->filter(account))
            continue;
        g_ptr_array_add(ac->accounts, account);
        g_ptr_array_add(ac->labels, dwp_account_display_name(account));
    }
    for(z = 0; z < (int)ac->accounts->len; z++)
        if(g_ptr_array_index(ac->accounts, z) == current)
            sel = z;
    if(sel < 0 && ac->accounts->len)
        sel = 0;
    dwp_combo_fill(ac->combo, (char **)ac->labels->pdata, (int)ac->labels->len, sel);
    ac->filling = FALSE;
}

PurpleAccount *dwp_account_combo_get(DwpAccountCombo *ac)
{
    int index;
    PurpleAccount *account;

    if(!ac->accounts || !ac->accounts->len)
        return NULL;
    index = dwp_combo_index(ac->combo, (char **)ac->labels->pdata, (int)ac->labels->len);
    if(index < 0)
        return NULL;
    account = g_ptr_array_index(ac->accounts, index);
    return dwp_account_exists(account) ? account : NULL;
}

void dwp_account_combo_set(DwpAccountCombo *ac, PurpleAccount *account)
{
    int z;

    for(z = 0; z < (int)ac->accounts->len; z++)
    {
        if(g_ptr_array_index(ac->accounts, z) == account)
        {
            ac->filling = TRUE;
            dw_listbox_select(ac->combo, z, TRUE);
            dw_window_set_text(ac->combo, g_ptr_array_index(ac->labels, z));
            ac->filling = FALSE;
            return;
        }
    }
}

void dwp_account_combo_on_change(DwpAccountCombo *ac, DwpAccountChanged cb, gpointer data)
{
    ac->changed = cb;
    ac->changed_data = data;
}

void dwp_account_combo_free(DwpAccountCombo *ac)
{
    if(!ac)
        return;
    purple_signals_disconnect_by_handle(&ac->handle);
    ac->changed = NULL;
    g_ptr_array_free(ac->accounts, TRUE);
    g_ptr_array_free(ac->labels, TRUE);
    /* The widget belongs to its window; signal data must stay valid until
     * that window is destroyed, so release the struct from the task queue. */
    dwp_defer(g_free, ac);
}

/* =========================================================== group combo */

HWND dwp_group_combo_new(const char *initial)
{
    HWND combo = dw_combobox_new(initial ? initial : "", 0);
    PurpleBlistNode *node;

    for(node = purple_blist_get_root(); node; node = purple_blist_node_get_sibling_next(node))
        if(PURPLE_BLIST_NODE_IS_GROUP(node))
            dw_listbox_append(combo, purple_group_get_name((PurpleGroup *)node));
    if(!initial || !*initial)
    {
        PurpleBlistNode *first = purple_blist_get_root();

        dw_window_set_text(combo, first && PURPLE_BLIST_NODE_IS_GROUP(first)
                                  ? purple_group_get_name((PurpleGroup *)first) : "Buddies");
    }
    return combo;
}

PurpleGroup *dwp_group_find_or_create(const char *name)
{
    PurpleGroup *group;

    if(!name || !*name)
        name = "Buddies";
    group = purple_find_group(name);
    if(!group)
    {
        group = purple_group_new(name);
        purple_blist_add_group(group, NULL);
    }
    return group;
}
