/*
 * DWPurple - libpurple core start-up/shutdown, account and connection UI ops.
 */
#include "dwpurple.h"

#define RECONNECT_INITIAL_MS 8000
#define RECONNECT_MAX_MS (4 * 60 * 1000)

static gboolean shutting_down = FALSE;
static int signal_handle;

/* ============================================================ reconnects */

typedef struct {
    PurpleAccount *account;
    guint source;
    int delay;
} Reconnect;

static GHashTable *reconnects = NULL;       /* account -> Reconnect */

typedef struct {
    PurpleAccount *account;
    HWND window;
} ErrorBox;

static GHashTable *error_boxes = NULL;      /* account -> ErrorBox */

static void cancel_reconnect(PurpleAccount *account)
{
    Reconnect *r;

    if(!reconnects || !(r = g_hash_table_lookup(reconnects, account)))
        return;
    if(r->source)
        purple_timeout_remove(r->source);
    g_hash_table_remove(reconnects, account);
}

static void do_reconnect(PurpleAccount *account)
{
    PurpleSavedStatus *saved;

    if(!dwp_account_exists(account) || !purple_account_get_enabled(account, DWP_UI_ID))
    {
        cancel_reconnect(account);
        return;
    }
    saved = purple_savedstatus_get_current();
    if(saved && purple_savedstatus_get_type(saved) == PURPLE_STATUS_OFFLINE)
    {
        cancel_reconnect(account);
        return;
    }
    if(!purple_account_is_disconnected(account))
        return;
    purple_account_connect(account);
}

static gboolean reconnect_timeout(gpointer data)
{
    Reconnect *r = data;

    r->source = 0;
    do_reconnect(r->account);
    return FALSE;
}

static void schedule_reconnect(PurpleAccount *account)
{
    Reconnect *r;
    int jitter;
    char *name, *msg;

    if(!reconnects)
        reconnects = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    r = g_hash_table_lookup(reconnects, account);
    if(!r)
    {
        r = g_new0(Reconnect, 1);
        r->account = account;
        r->delay = RECONNECT_INITIAL_MS;
        g_hash_table_insert(reconnects, account, r);
    }
    else
    {
        r->delay = MIN(r->delay * 2, RECONNECT_MAX_MS);
        if(r->source)
            purple_timeout_remove(r->source);
    }
    jitter = g_random_int_range(0, r->delay / 4 + 1);
    r->source = purple_timeout_add(r->delay + jitter, reconnect_timeout, r);
    name = dwp_account_display_name(account);
    msg = g_strdup_printf("%s: reconnecting in %d seconds", name, (r->delay + jitter) / 1000);
    dwp_blist_status_message(msg);
    g_free(msg);
    g_free(name);
}

void dwp_core_reconnect_now(PurpleAccount *account)
{
    cancel_reconnect(account);
    do_reconnect(account);
}

static void error_box_close(PurpleAccount *account)
{
    ErrorBox *box;

    if(!error_boxes || !(box = g_hash_table_lookup(error_boxes, account)))
        return;
    g_hash_table_steal(error_boxes, account);
    dwp_window_close_later(box->window, g_free, box);
}

static void on_account_gone(PurpleAccount *account, gpointer unused)
{
    (void)unused;
    cancel_reconnect(account);
    error_box_close(account);
}

/* ============================================================== core ops */

static void ui_prefs_init(void)
{
    purple_prefs_add_none(DWP_PREFS_ROOT);
    purple_prefs_add_none(DWP_PREF_PLUGINS);
    purple_prefs_add_path_list(DWP_PREF_LOADED, NULL);
    purple_prefs_add_none(DWP_PREF_BLIST);
    purple_prefs_add_bool(DWP_PREF_OFFLINE, FALSE);
    purple_prefs_add_bool(DWP_PREF_EMPTY, FALSE);
    purple_prefs_add_int(DWP_PREF_BLIST_W, 300);
    purple_prefs_add_int(DWP_PREF_BLIST_H, 560);
    purple_prefs_add_none(DWP_PREF_CONV);
    purple_prefs_add_int(DWP_PREF_CONV_W, 680);
    purple_prefs_add_int(DWP_PREF_CONV_H, 480);
    purple_prefs_add_bool(DWP_PREF_TIMESTAMPS, TRUE);
    purple_prefs_add_bool(DWP_PREF_NOTIFY_MSGS, FALSE);
    purple_prefs_add_bool(DWP_PREF_RAISE_NEW, TRUE);
    purple_prefs_add_none(DWP_PREF_TRAY);
    purple_prefs_add_bool(DWP_PREF_TRAY_ENABLED, TRUE);
    dwp_sound_add_prefs();
    dwp_emoji_add_prefs();
}

static void debug_ui_init(void)
{
    purple_debug_set_ui_ops(dwp_debug_ui_ops());
}

static PurpleAccountUiOps *account_ui_ops(void);
static PurpleConnectionUiOps *connection_ui_ops(void);

static void ui_init(void)
{
    void *accounts = purple_accounts_get_handle();

    purple_accounts_set_ui_ops(account_ui_ops());
    purple_connections_set_ui_ops(connection_ui_ops());
    purple_blist_set_ui_ops(dwp_blist_ui_ops());
    purple_conversations_set_ui_ops(dwp_conv_ui_ops());
    purple_request_set_ui_ops(dwp_request_ui_ops());
    purple_notify_set_ui_ops(dwp_notify_ui_ops());
    purple_xfers_set_ui_ops(dwp_xfer_ui_ops());
    purple_roomlist_set_ui_ops(dwp_roomlist_ui_ops());
    purple_privacy_set_ui_ops(dwp_privacy_ui_ops());
    purple_sound_set_ui_ops(dwp_sound_ui_ops());
    purple_idle_set_ui_ops(dwp_idle_ui_ops());
    dwp_pounces_init();

    purple_signal_connect(accounts, "account-disabled", &signal_handle,
                          PURPLE_CALLBACK(on_account_gone), NULL);
    purple_signal_connect(accounts, "account-removed", &signal_handle,
                          PURPLE_CALLBACK(on_account_gone), NULL);

    dwp_blist_create();
    dwp_conv_init();
    dwp_irc_console_init();
}

static void core_quit(void)
{
}

static GHashTable *get_ui_info(void)
{
    static GHashTable *info = NULL;

    if(!info)
    {
        info = g_hash_table_new(g_str_hash, g_str_equal);
        g_hash_table_insert(info, (gpointer)"name", (gpointer)DWP_NAME);
        g_hash_table_insert(info, (gpointer)"version", (gpointer)DWP_VERSION);
        g_hash_table_insert(info, (gpointer)"website", (gpointer)DWP_WEBSITE);
        g_hash_table_insert(info, (gpointer)"dev_website", (gpointer)DWP_WEBSITE);
        g_hash_table_insert(info, (gpointer)"client_type", (gpointer)"pc");
    }
    return info;
}

static PurpleCoreUiOps *core_ui_ops(void)
{
    static PurpleCoreUiOps ops;
    static gboolean ready = FALSE;

    if(!ready)
    {
        memset(&ops, 0, sizeof(ops));
        ops.ui_prefs_init = ui_prefs_init;
        ops.debug_ui_init = debug_ui_init;
        ops.ui_init = ui_init;
        ops.quit = core_quit;
        ops.get_ui_info = get_ui_info;
        ready = TRUE;
    }
    return &ops;
}

/* =========================================================== account ops */

static char *who_string(const char *remote_user, const char *alias)
{
    if(alias && *alias)
        return g_strdup_printf("%s (%s)", alias, remote_user);
    return g_strdup(remote_user);
}

static void notify_added(PurpleAccount *account, const char *remote_user, const char *id,
                         const char *alias, const char *message)
{
    char *who = who_string(remote_user, alias);
    char *primary = g_strdup_printf("%s has made %s their buddy.", who,
                                    id && *id ? id : purple_account_get_username(account));

    purple_notify_info(account, "Buddy added", primary, message);
    g_free(primary);
    g_free(who);
}

typedef struct {
    PurpleAccount *account;
    char *user, *alias;
} AddRequest;

static void add_request_free(AddRequest *req)
{
    g_free(req->user);
    g_free(req->alias);
    g_free(req);
}

static void add_request_add(AddRequest *req, int unused)
{
    (void)unused;
    if(dwp_account_exists(req->account))
        dwp_show_add_buddy(req->account, req->user, NULL, req->alias);
    add_request_free(req);
}

static void add_request_cancel(AddRequest *req, int unused)
{
    (void)unused;
    add_request_free(req);
}

static void request_add(PurpleAccount *account, const char *remote_user, const char *id,
                        const char *alias, const char *message)
{
    AddRequest *req = g_new0(AddRequest, 1);
    char *who = who_string(remote_user, alias);
    char *primary = g_strdup_printf("%s has made %s their buddy. Add them to your buddy list?", who,
                                    id && *id ? id : purple_account_get_username(account));

    req->account = account;
    req->user = g_strdup(remote_user);
    req->alias = g_strdup(alias);
    purple_request_action(account, "Add buddy", primary, message, 0,
                          account, remote_user, NULL, req, 2,
                          "_Add", G_CALLBACK(add_request_add),
                          "_Cancel", G_CALLBACK(add_request_cancel));
    g_free(primary);
    g_free(who);
}

typedef struct {
    HWND window, addcheck;
    PurpleAccount *account;
    char *user, *alias;
    PurpleAccountRequestAuthorizationCb authorize_cb, deny_cb;
    void *user_data;
    gboolean done;
} AuthBox;

static GList *auth_boxes = NULL;

static void auth_box_free(gpointer p)
{
    AuthBox *box = p;

    g_free(box->user);
    g_free(box->alias);
    g_free(box);
}

static void auth_finish(AuthBox *box, gboolean authorized)
{
    gboolean want_add;
    PurpleAccount *account = box->account;
    char *user, *alias;

    if(box->done || !g_list_find(auth_boxes, box))
        return;
    box->done = TRUE;
    auth_boxes = g_list_remove(auth_boxes, box);
    want_add = authorized && box->addcheck && dw_checkbox_get(box->addcheck);
    user = g_strdup(box->user);
    alias = g_strdup(box->alias);
    /* libpurple frees its request record inside these callbacks. */
    if(authorized)
    {
        if(box->authorize_cb)
            box->authorize_cb(box->user_data);
    }
    else if(box->deny_cb)
        box->deny_cb(box->user_data);
    dwp_window_close_later(box->window, auth_box_free, box);
    if(want_add && dwp_account_exists(account))
        dwp_show_add_buddy(account, user, NULL, alias);
    g_free(user);
    g_free(alias);
}

static int DWSIGNAL auth_authorize(HWND w, void *data)
{
    (void)w;
    auth_finish(data, TRUE);
    return TRUE;
}

static int DWSIGNAL auth_deny(HWND w, void *data)
{
    (void)w;
    auth_finish(data, FALSE);
    return TRUE;
}

static void *request_authorize(PurpleAccount *account, const char *remote_user, const char *id,
                               const char *alias, const char *message, gboolean on_list,
                               PurpleAccountRequestAuthorizationCb authorize_cb,
                               PurpleAccountRequestAuthorizationCb deny_cb, void *user_data)
{
    AuthBox *box = g_new0(AuthBox, 1);
    char *who = who_string(remote_user, alias);
    char *text = g_strdup_printf("%s wants to add %s to their buddy list.", who,
                                 id && *id ? id : purple_account_get_username(account));
    HWND vbox, buttons, authb;

    box->account = account;
    box->user = g_strdup(remote_user);
    box->alias = g_strdup(alias);
    box->authorize_cb = authorize_cb;
    box->deny_cb = deny_cb;
    box->user_data = user_data;
    box->window = dwp_window_new("Authorize buddy?", 0, 0);
    vbox = dw_box_new(DW_VERT, 4);
    dw_box_pack_start(box->window, vbox, 0, 0, TRUE, TRUE, 0);
    dw_box_pack_start(vbox, dwp_label(text), 420, DW_SIZE_AUTO, TRUE, FALSE, 4);
    if(message && *message)
    {
        char *m = g_strdup_printf("Message: %s", message);

        dw_box_pack_start(vbox, dwp_label(m), 420, DW_SIZE_AUTO, TRUE, FALSE, 4);
        g_free(m);
    }
    if(!on_list)
    {
        box->addcheck = dw_checkbox_new("Add to my buddy list", 0);
        dw_checkbox_set(box->addcheck, TRUE);
        dw_box_pack_start(vbox, box->addcheck, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 4);
    }
    buttons = dw_box_new(DW_HORZ, 0);
    dw_box_pack_start(vbox, buttons, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 2);
    dwp_spacer(buttons);
    authb = dwp_button(buttons, "Authorize", DW_SIGNAL_FUNC(auth_authorize), box);
    dwp_button(buttons, "Deny", DW_SIGNAL_FUNC(auth_deny), box);
    dw_signal_connect(box->window, DW_SIGNAL_DELETE, DW_SIGNAL_FUNC(auth_deny), box);
    auth_boxes = g_list_append(auth_boxes, box);
    dwp_window_present(box->window, 460, 170);
    dw_window_set_focus(authb);
    g_free(text);
    g_free(who);
    return box;
}

static void close_account_request(void *ui_handle)
{
    AuthBox *box = ui_handle;

    if(!g_list_find(auth_boxes, box))
        return;
    auth_boxes = g_list_remove(auth_boxes, box);
    box->done = TRUE;
    dwp_window_close_later(box->window, auth_box_free, box);
}

static PurpleAccountUiOps *account_ui_ops(void)
{
    static PurpleAccountUiOps ops;
    static gboolean ready = FALSE;

    if(!ready)
    {
        memset(&ops, 0, sizeof(ops));
        ops.notify_added = notify_added;
        ops.request_add = request_add;
        ops.request_authorize = request_authorize;
        ops.close_account_request = close_account_request;
        ready = TRUE;
    }
    return &ops;
}

/* ======================================================== connection ops */

static void connect_progress(PurpleConnection *gc, const char *text, size_t step, size_t step_count)
{
    char *name = dwp_account_display_name(purple_connection_get_account(gc));
    char *msg;

    if(step_count > 0)
        msg = g_strdup_printf("%s: %s (%lu/%lu)", name, text ? text : "",
                              (unsigned long)step + 1, (unsigned long)step_count);
    else
        msg = g_strdup_printf("%s: %s", name, text ? text : "");
    dwp_blist_status_message(msg);
    g_free(msg);
    g_free(name);
}

static void connected(PurpleConnection *gc)
{
    PurpleAccount *account = purple_connection_get_account(gc);
    char *name = dwp_account_display_name(account);
    char *msg = g_strdup_printf("%s: connected", name);

    cancel_reconnect(account);
    error_box_close(account);
    dwp_blist_status_message(msg);
    g_free(msg);
    g_free(name);
}

static void disconnected(PurpleConnection *gc)
{
    (void)gc;
}

static int DWSIGNAL error_reconnect(HWND w, void *data)
{
    ErrorBox *box = data;
    PurpleAccount *account = box->account;

    (void)w;
    error_box_close(account);
    if(dwp_account_exists(account))
    {
        if(!purple_account_get_enabled(account, DWP_UI_ID))
            purple_account_set_enabled(account, DWP_UI_ID, TRUE);
        dwp_core_reconnect_now(account);
    }
    return TRUE;
}

static int DWSIGNAL error_modify(HWND w, void *data)
{
    ErrorBox *box = data;
    PurpleAccount *account = box->account;

    (void)w;
    error_box_close(account);
    if(dwp_account_exists(account))
        dwp_account_edit(account);
    return TRUE;
}

static int DWSIGNAL error_close(HWND w, void *data)
{
    ErrorBox *box = data;

    (void)w;
    error_box_close(box->account);
    return TRUE;
}

static void show_fatal_error(PurpleAccount *account, const char *text)
{
    ErrorBox *box;
    HWND vbox, buttons;
    char *name, *primary;

    if(!error_boxes)
        error_boxes = g_hash_table_new(g_direct_hash, g_direct_equal);
    error_box_close(account);
    box = g_new0(ErrorBox, 1);
    box->account = account;
    name = dwp_account_display_name(account);
    primary = g_strdup_printf("%s disconnected", name);
    box->window = dwp_window_new("Connection error", 0, 0);
    vbox = dw_box_new(DW_VERT, 4);
    dw_box_pack_start(box->window, vbox, 0, 0, TRUE, TRUE, 0);
    dw_box_pack_start(vbox, dwp_label(primary), 440, DW_SIZE_AUTO, TRUE, FALSE, 4);
    dw_box_pack_start(vbox, dwp_label(text), 440, DW_SIZE_AUTO, TRUE, FALSE, 4);
    dw_box_pack_start(vbox, dwp_label("The account will not reconnect automatically."),
                      440, DW_SIZE_AUTO, TRUE, FALSE, 4);
    buttons = dw_box_new(DW_HORZ, 0);
    dw_box_pack_start(vbox, buttons, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 2);
    dwp_spacer(buttons);
    dwp_button(buttons, "Reconnect", DW_SIGNAL_FUNC(error_reconnect), box);
    dwp_button(buttons, "Modify Account", DW_SIGNAL_FUNC(error_modify), box);
    dwp_button(buttons, "Close", DW_SIGNAL_FUNC(error_close), box);
    dw_signal_connect(box->window, DW_SIGNAL_DELETE, DW_SIGNAL_FUNC(error_close), box);
    g_hash_table_insert(error_boxes, account, box);
    dwp_window_present(box->window, 480, 170);
    g_free(primary);
    g_free(name);
}

static void report_disconnect_reason(PurpleConnection *gc, PurpleConnectionError reason, const char *text)
{
    PurpleAccount *account = purple_connection_get_account(gc);
    char *name = dwp_account_display_name(account);
    char *msg = g_strdup_printf("%s: %s", name, text ? text : "");

    dwp_blist_status_message(msg);
    g_free(msg);
    g_free(name);
    if(shutting_down)
        return;
    if(!purple_connection_error_is_fatal(reason))
        schedule_reconnect(account);
    else
        show_fatal_error(account, text ? text : "");
}

static void network_connected(void)
{
    GList *pending, *l;

    if(!reconnects)
        return;
    pending = g_hash_table_get_keys(reconnects);
    for(l = pending; l; l = l->next)
        do_reconnect(l->data);
    g_list_free(pending);
}

static void network_disconnected(void)
{
    GList *active = purple_accounts_get_all_active(), *l;

    for(l = active; l; l = l->next)
    {
        PurpleAccount *account = l->data;
        char *password;

        if(purple_account_is_disconnected(account))
            continue;
        /* Keep the password across the forced disconnect. */
        password = g_strdup(purple_account_get_password(account));
        purple_account_disconnect(account);
        purple_account_set_password(account, password);
        g_free(password);
        schedule_reconnect(account);
    }
    g_list_free(active);
}

static PurpleConnectionUiOps *connection_ui_ops(void)
{
    static PurpleConnectionUiOps ops;
    static gboolean ready = FALSE;

    if(!ready)
    {
        memset(&ops, 0, sizeof(ops));
        ops.connect_progress = connect_progress;
        ops.connected = connected;
        ops.disconnected = disconnected;
        ops.network_connected = network_connected;
        ops.network_disconnected = network_disconnected;
        ops.report_disconnect_reason = report_disconnect_reason;
        ready = TRUE;
    }
    return &ops;
}

/* ================================================================ public */

gboolean dwp_core_init(const DwpOptions *options, char **error)
{
    char *dir, *plugins;

    if(options->config_dir && *options->config_dir)
        dir = g_strdup(options->config_dir);
    else
        dir = g_build_filename(g_get_home_dir(), ".dwpurple", NULL);
    g_mkdir_with_parents(dir, 0700);
    purple_util_set_user_dir(dir);
    g_free(dir);

    purple_debug_set_enabled(options->debug ? TRUE : FALSE);
    purple_core_set_ui_ops(core_ui_ops());
    purple_eventloop_set_ui_ops(dwp_eventloop_ui_ops());
    dwp_eventloop_start();
    dwp_tasks_start();

    plugins = g_build_filename(purple_user_dir(), "plugins", NULL);
    purple_plugins_add_search_path(plugins);
    g_free(plugins);

    if(!purple_core_init(DWP_UI_ID))
    {
        if(error)
            *error = g_strdup("libpurple initialization failed.");
        return FALSE;
    }
    if(!purple_core_ensure_single_instance())
    {
        if(error)
            *error = g_strdup("Another libpurple client is already running with this configuration directory.");
        purple_core_quit();
        return FALSE;
    }

    purple_set_blist(purple_blist_new());
    purple_blist_load();
    purple_prefs_load();
    purple_prefs_update_old();
    purple_plugins_load_saved(DWP_PREF_LOADED);
    purple_pounces_load();

    dwp_blist_apply_prefs();
    purple_blist_show();
    dwp_tray_apply_prefs();

    if(options->no_login)
    {
        PurpleSavedStatus *off =
            purple_savedstatus_find_transient_by_type_and_message(PURPLE_STATUS_OFFLINE, NULL);

        if(!off)
            off = purple_savedstatus_new(NULL, PURPLE_STATUS_OFFLINE);
        purple_savedstatus_activate(off);
    }
    else
    {
        purple_savedstatus_activate(purple_savedstatus_get_startup());
        purple_accounts_restore_current_statuses();
    }

    if(!purple_accounts_get_all())
        dwp_accounts_show();
    return TRUE;
}

void dwp_core_request_quit(void)
{
    dw_main_quit();
}

gboolean dwp_core_shutting_down(void)
{
    return shutting_down;
}

void dwp_core_shutdown(void)
{
    GList *pending, *l;

    if(shutting_down)
        return;
    shutting_down = TRUE;
    if(reconnects)
    {
        pending = g_hash_table_get_keys(reconnects);
        for(l = pending; l; l = l->next)
            cancel_reconnect(l->data);
        g_list_free(pending);
    }
    purple_signals_disconnect_by_handle(&signal_handle);
    dwp_blist_save_geometry();
    dwp_conv_save_geometry();
    dwp_tray_shutdown();
    purple_core_quit();
    dwp_idle_shutdown();
    dwp_tasks_stop();
    dwp_eventloop_shutdown();
}
