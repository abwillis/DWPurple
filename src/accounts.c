/*
 * DWPurple - account manager, account editor and account action menus.
 */
#include "dwpurple.h"

/* =========================================================== action menus */

static GPtrArray *plugin_actions = NULL;     /* PurplePluginAction, popup lifetime */

static void reset_plugin_actions(void)
{
    if(!plugin_actions)
        plugin_actions = g_ptr_array_new_with_free_func((GDestroyNotify)purple_plugin_action_free);
    g_ptr_array_set_size(plugin_actions, 0);
}

static void act_edit(gpointer account, gpointer b, const char *s)
{
    (void)b; (void)s;
    if(dwp_account_exists(account))
        dwp_account_edit(account);
}

static void act_privacy(gpointer account, gpointer b, const char *s)
{
    (void)b; (void)s;
    if(dwp_account_exists(account))
        dwp_privacy_show(account);
}

static void act_console(gpointer account, gpointer b, const char *s)
{
    (void)b; (void)s;
    dwp_irc_console_show(account);
}

static void act_connect(gpointer account, gpointer b, const char *s)
{
    (void)b; (void)s;
    if(dwp_account_exists(account))
        dwp_core_reconnect_now(account);
}

static void set_enabled(PurpleAccount *account, gboolean enable)
{
    if(!dwp_account_exists(account) || !!purple_account_get_enabled(account, DWP_UI_ID) == !!enable)
        return;
    if(enable)
    {
        /* Make sure the account comes up with the current global status. */
        PurpleSavedStatus *saved = purple_savedstatus_get_current();

        if(saved)
            purple_savedstatus_activate_for_account(saved, account);
    }
    purple_account_set_enabled(account, DWP_UI_ID, enable);
}

static void act_enable(gpointer account, gpointer enable, const char *s)
{
    (void)s;
    set_enabled(account, GPOINTER_TO_INT(enable));
}

static void act_plugin(gpointer act, gpointer gc, const char *s)
{
    PurplePluginAction *pa = act;

    (void)s;
    if(dwp_connection_exists(gc) && pa->callback)
        pa->callback(pa);
}

static void add_account_items(HMENUI sub, PurpleAccount *account)
{
    PurpleConnection *gc = purple_account_get_connection(account);

    dwp_menu_add(sub, "~Edit Account", act_edit, account, NULL, NULL);
    dwp_menu_add(sub, "~Privacy...", act_privacy, account, NULL, NULL);
    if(dwp_irc_console_supported(account))
        dwp_menu_add(sub, "Server ~Console", act_console, account, NULL, NULL);
    if(gc && purple_account_is_connected(account))
    {
        PurplePlugin *plugin = purple_connection_get_prpl(gc);

        if(plugin && PURPLE_PLUGIN_HAS_ACTIONS(plugin))
        {
            GList *actions = PURPLE_PLUGIN_ACTIONS(plugin, gc), *a;

            if(actions)
                dwp_menu_separator(sub);
            for(a = actions; a; a = a->next)
            {
                PurplePluginAction *pa = a->data;
                char *label;

                if(!pa)
                {
                    dwp_menu_separator(sub);
                    continue;
                }
                pa->plugin = plugin;
                pa->context = gc;
                g_ptr_array_add(plugin_actions, pa);
                label = dwp_mnemonic(pa->label);
                dwp_menu_add(sub, label, act_plugin, pa, gc, NULL);
                g_free(label);
            }
            g_list_free(actions);
        }
    }
    else if(!gc && purple_account_get_enabled(account, DWP_UI_ID))
        dwp_menu_add(sub, "~Connect", act_connect, account, NULL, NULL);
    dwp_menu_separator(sub);
    if(purple_account_get_enabled(account, DWP_UI_ID))
        dwp_menu_add(sub, "~Disable", act_enable, account, GINT_TO_POINTER(FALSE), NULL);
    else
        dwp_menu_add(sub, "~Enable", act_enable, account, GINT_TO_POINTER(TRUE), NULL);
}

void dwp_account_actions_menu(PurpleAccount *account, HWND parent)
{
    HMENUI menu;

    if(!dwp_account_exists(account))
        return;
    dwp_actions_reset();
    reset_plugin_actions();
    menu = dw_menu_new(0);
    add_account_items(menu, account);
    dwp_menu_popup_here(&menu, parent);
}

void dwp_accounts_popup(HWND parent)
{
    HMENUI menu;
    GList *l;

    dwp_actions_reset();
    reset_plugin_actions();
    menu = dw_menu_new(0);
    for(l = purple_accounts_get_all(); l; l = l->next)
    {
        PurpleAccount *account = l->data;
        HMENUI sub = dw_menu_new(0);
        char *name = dwp_account_display_name(account);
        char *label = g_strdup_printf("%s - %s", name,
                                      purple_account_get_enabled(account, DWP_UI_ID)
                                      ? dwp_primitive_label(dwp_account_primitive(account)) : "Disabled");

        add_account_items(sub, account);
        dw_menu_append_item(menu, label, DW_MENU_POPUP, 0, TRUE, FALSE, sub);
        g_free(label);
        g_free(name);
    }
    if(!purple_accounts_get_all())
        dw_menu_append_item(menu, "(no accounts)", DW_MENU_POPUP, 0, TRUE, FALSE, DW_NOMENU);
    dwp_menu_popup_here(&menu, parent);
}

/* ======================================================== accounts window */

typedef struct {
    HWND window, list;
    gboolean refresh_pending;
} AccountsDlg;

static AccountsDlg *accounts_dlg = NULL;
static int accounts_handle;

static char *account_status_text(PurpleAccount *account)
{
    const PurpleConnectionErrorInfo *err;

    if(!purple_account_get_enabled(account, DWP_UI_ID))
        return g_strdup("Disabled");
    if(purple_account_is_connected(account))
    {
        PurpleStatus *status = purple_account_get_active_status(account);

        return g_strdup(status ? purple_status_get_name(status) : "Online");
    }
    if(purple_account_is_connecting(account))
        return g_strdup("Connecting...");
    err = purple_account_get_current_error(account);
    if(err && err->description)
        return g_strdup_printf("Error: %s", err->description);
    return g_strdup("Offline");
}

static PurpleAccount *selected_account(void)
{
    PurpleAccount *account;

    if(!accounts_dlg)
        return NULL;
    account = (PurpleAccount *)dw_container_query_start(accounts_dlg->list, DW_CRA_SELECTED | DW_CR_RETDATA);
    return dwp_account_exists(account) ? account : NULL;
}

static void accounts_refresh(gpointer unused)
{
    PurpleAccount *selected;
    GList *l;
    int count = 0, z = 0;
    void *alloc;

    (void)unused;
    if(!accounts_dlg)
        return;
    accounts_dlg->refresh_pending = FALSE;
    selected = selected_account();
    dw_container_clear(accounts_dlg->list, TRUE);
    count = (int)g_list_length(purple_accounts_get_all());
    if(!count)
        return;
    alloc = dw_container_alloc(accounts_dlg->list, count);
    for(l = purple_accounts_get_all(); l; l = l->next, z++)
    {
        PurpleAccount *account = l->data;
        char *enabled = purple_account_get_enabled(account, DWP_UI_ID) ? "Yes" : "No";
        char *user = (char *)purple_account_get_username(account);
        char *proto = (char *)purple_account_get_protocol_name(account);
        char *status = account_status_text(account);

        dw_container_set_item(accounts_dlg->list, alloc, 0, z, &enabled);
        dw_container_set_item(accounts_dlg->list, alloc, 1, z, &user);
        dw_container_set_item(accounts_dlg->list, alloc, 2, z, &proto);
        dw_container_set_item(accounts_dlg->list, alloc, 3, z, &status);
        dw_container_set_row_title(alloc, z, user);
        dw_container_set_row_data(alloc, z, account);
        g_free(status);
    }
    dw_container_insert(accounts_dlg->list, alloc, count);
    dw_container_optimize(accounts_dlg->list);
    if(selected)
        dw_container_cursor_by_data(accounts_dlg->list, selected);
}

static void schedule_refresh(void)
{
    if(!accounts_dlg || accounts_dlg->refresh_pending)
        return;
    accounts_dlg->refresh_pending = TRUE;
    dwp_defer(accounts_refresh, NULL);
}

static void accounts_changed(PurpleAccount *account, gpointer data)
{
    (void)account;
    (void)data;
    schedule_refresh();
}

static void account_removed(PurpleAccount *account, gpointer data)
{
    (void)account;
    (void)data;
    /* The account is destroyed right after this: drop rows synchronously. */
    if(accounts_dlg)
        dw_container_clear(accounts_dlg->list, TRUE);
    schedule_refresh();
}

static int DWSIGNAL acc_add(HWND w, void *data)
{
    (void)w; (void)data;
    dwp_account_edit(NULL);
    return TRUE;
}

static int DWSIGNAL acc_modify(HWND w, void *data)
{
    PurpleAccount *account = selected_account();

    (void)w; (void)data;
    if(account)
        dwp_account_edit(account);
    return TRUE;
}

static int DWSIGNAL acc_enter(HWND w, char *text, void *data, void *itemdata)
{
    (void)w; (void)text; (void)data;
    if(dwp_account_exists(itemdata))
        dwp_account_edit(itemdata);
    return TRUE;
}

static int DWSIGNAL acc_delete(HWND w, void *data)
{
    PurpleAccount *account = selected_account();
    char *name;
    gboolean ok;

    (void)w; (void)data;
    if(!account)
        return TRUE;
    name = dwp_account_display_name(account);
    ok = dwp_confirm("Delete Account", "Delete the account %s?\n\nIts buddies and logs on this "
                     "computer will no longer be associated with it.", name);
    g_free(name);
    if(ok && dwp_account_exists(account))
        purple_accounts_delete(account);
    schedule_refresh();
    return TRUE;
}

static int DWSIGNAL acc_toggle(HWND w, void *data)
{
    PurpleAccount *account = selected_account();

    (void)w; (void)data;
    if(account)
        set_enabled(account, !purple_account_get_enabled(account, DWP_UI_ID));
    schedule_refresh();
    return TRUE;
}

static int DWSIGNAL acc_actions(HWND w, void *data)
{
    PurpleAccount *account = selected_account();

    (void)w; (void)data;
    if(account)
        dwp_account_actions_menu(account, accounts_dlg->window);
    return TRUE;
}

static int DWSIGNAL acc_context(HWND w, char *text, int x, int y, void *data, void *itemdata)
{
    (void)w; (void)text; (void)x; (void)y; (void)data;
    if(dwp_account_exists(itemdata))
        dwp_account_actions_menu(itemdata, accounts_dlg->window);
    return TRUE;
}

static void accounts_dlg_free(gpointer p)
{
    g_free(p);
}

static int DWSIGNAL acc_close(HWND w, void *data)
{
    AccountsDlg *d = accounts_dlg;

    (void)w; (void)data;
    if(!d)
        return TRUE;
    accounts_dlg = NULL;
    purple_signals_disconnect_by_handle(&accounts_handle);
    dwp_window_close_later(d->window, accounts_dlg_free, d);
    return TRUE;
}

void dwp_accounts_show(void)
{
    static const char *const sigs[] = {
        "account-added", "account-enabled", "account-disabled", "account-status-changed",
        "account-connecting", "account-signed-on", "account-signed-off",
        "account-error-changed", "account-alias-changed", NULL
    };
    unsigned long flags[4] = {
        DW_CFA_STRING | DW_CFA_CENTER | DW_CFA_HORZSEPARATOR | DW_CFA_SEPARATOR,
        DW_CFA_STRING | DW_CFA_LEFT | DW_CFA_HORZSEPARATOR | DW_CFA_SEPARATOR,
        DW_CFA_STRING | DW_CFA_LEFT | DW_CFA_HORZSEPARATOR | DW_CFA_SEPARATOR,
        DW_CFA_STRING | DW_CFA_LEFT | DW_CFA_HORZSEPARATOR | DW_CFA_SEPARATOR
    };
    char *titles[4] = { "Enabled", "Username", "Protocol", "Status" };
    AccountsDlg *d;
    HWND box, buttons;
    void *h = purple_accounts_get_handle();
    int z;

    if(accounts_dlg)
    {
        dw_window_show(accounts_dlg->window);
        dw_window_raise(accounts_dlg->window);
        return;
    }
    d = accounts_dlg = g_new0(AccountsDlg, 1);
    d->window = dwp_window_new("Accounts", 0, 0);
    box = dw_box_new(DW_VERT, 4);
    dw_box_pack_start(d->window, box, 0, 0, TRUE, TRUE, 0);
    d->list = dw_container_new(0, FALSE);
    dw_container_setup(d->list, flags, titles, 4, 0);
    dw_box_pack_start(box, d->list, 520, 220, TRUE, TRUE, 2);
    dw_signal_connect(d->list, DW_SIGNAL_ITEM_ENTER, DW_SIGNAL_FUNC(acc_enter), NULL);
    dw_signal_connect(d->list, DW_SIGNAL_ITEM_CONTEXT, DW_SIGNAL_FUNC(acc_context), NULL);

    buttons = dw_box_new(DW_HORZ, 0);
    dw_box_pack_start(box, buttons, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 2);
    dwp_button(buttons, "Add...", DW_SIGNAL_FUNC(acc_add), NULL);
    dwp_button(buttons, "Modify...", DW_SIGNAL_FUNC(acc_modify), NULL);
    dwp_button(buttons, "Delete", DW_SIGNAL_FUNC(acc_delete), NULL);
    dwp_button(buttons, "Enable/Disable", DW_SIGNAL_FUNC(acc_toggle), NULL);
    dwp_button(buttons, "Actions...", DW_SIGNAL_FUNC(acc_actions), NULL);
    dwp_spacer(buttons);
    dwp_button(buttons, "Close", DW_SIGNAL_FUNC(acc_close), NULL);
    dw_signal_connect(d->window, DW_SIGNAL_DELETE, DW_SIGNAL_FUNC(acc_close), NULL);

    for(z = 0; sigs[z]; z++)
        purple_signal_connect(h, sigs[z], &accounts_handle, PURPLE_CALLBACK(accounts_changed), NULL);
    purple_signal_connect(h, "account-removed", &accounts_handle, PURPLE_CALLBACK(account_removed), NULL);

    accounts_refresh(NULL);
    dwp_window_present(d->window, 600, 320);
}

/* ========================================================= account editor */

typedef struct {
    PurpleAccountUserSplit *split;
    HWND entry;
} SplitWidget;

typedef struct {
    PurpleAccountOption *option;
    HWND widget;
    GPtrArray *values;      /* STRING_LIST: internal values, parallel to labels */
    GPtrArray *labels;
} OptionWidget;

typedef struct {
    PurpleAccount *account;
    HWND window, protocol, notebook;
    HWND basic_holder, basic_box, adv_holder, adv_box;
    HWND username, password, remember, alias, check_mail, reg;
    GPtrArray *prpl_ids, *prpl_names;
    GArray *splits;
    GPtrArray *options;
    char *saved_user, *saved_pass, *saved_alias;
    gboolean saved_remember;
    int last_protocol;
} Editor;

static void option_widget_free(gpointer p)
{
    OptionWidget *o = p;

    if(o->values)
        g_ptr_array_free(o->values, TRUE);
    if(o->labels)
        g_ptr_array_free(o->labels, TRUE);
    g_free(o);
}

static void editor_free(gpointer p)
{
    Editor *e = p;

    g_ptr_array_free(e->prpl_ids, TRUE);
    g_ptr_array_free(e->prpl_names, TRUE);
    g_array_free(e->splits, TRUE);
    g_ptr_array_free(e->options, TRUE);
    g_free(e->saved_user);
    g_free(e->saved_pass);
    g_free(e->saved_alias);
    g_free(e);
}

static PurplePlugin *editor_prpl(Editor *e)
{
    int index;

    if(e->account)
        return purple_find_prpl(purple_account_get_protocol_id(e->account));
    index = dwp_combo_index(e->protocol, (char **)e->prpl_names->pdata, (int)e->prpl_names->len);
    if(index < 0)
        return NULL;
    return purple_find_prpl(g_ptr_array_index(e->prpl_ids, index));
}

/* Remember what the user typed before rebuilding for another protocol. */
static void editor_save_basic(Editor *e)
{
    if(!e->username)
        return;
    g_free(e->saved_user);
    e->saved_user = dwp_get_text(e->username);
    if(e->password)
    {
        g_free(e->saved_pass);
        e->saved_pass = dwp_get_text(e->password);
        e->saved_remember = dw_checkbox_get(e->remember);
    }
    g_free(e->saved_alias);
    e->saved_alias = dwp_get_text(e->alias);
}

static void editor_build(Editor *e)
{
    PurplePlugin *prpl = editor_prpl(e);
    PurplePluginProtocolInfo *info = prpl ? PURPLE_PLUGIN_PROTOCOL_INFO(prpl) : NULL;
    char *username;
    GList *l;
    int z;
    gboolean no_password;

    editor_save_basic(e);
    g_array_set_size(e->splits, 0);
    g_ptr_array_set_size(e->options, 0);
    e->username = e->password = e->remember = e->alias = e->check_mail = e->reg = 0;
    e->basic_box = dwp_box_reset(e->basic_holder, e->basic_box, DW_VERT);
    e->adv_box = dwp_box_reset(e->adv_holder, e->adv_box, DW_VERT);

    /* ---- user splits ("user@server/resource") */
    username = g_strdup(e->account ? purple_account_get_username(e->account)
                                   : (e->saved_user ? e->saved_user : ""));
    e->username = dw_entryfield_new("", 0);
    dwp_form_row(e->basic_box, "Username:", e->username);
    for(l = info ? info->user_splits : NULL; l; l = l->next)
    {
        SplitWidget sw;
        char *label = dwp_strip_mnemonic(purple_account_user_split_get_text(l->data));
        char *withcolon = g_strdup_printf("%s:", label);

        sw.split = l->data;
        sw.entry = dw_entryfield_new("", 0);
        dwp_form_row(e->basic_box, withcolon, sw.entry);
        g_array_append_val(e->splits, sw);
        g_free(withcolon);
        g_free(label);
    }
    /* Peel split values off the end of the username, last split first. */
    for(z = (int)e->splits->len - 1; z >= 0; z--)
    {
        SplitWidget *sw = &g_array_index(e->splits, SplitWidget, z);
        char sep = purple_account_user_split_get_separator(sw->split);
        const char *value = NULL;
        char *pos = NULL;

        if(e->account)
            pos = purple_account_user_split_get_reverse(sw->split) ? strrchr(username, sep)
                                                                   : strchr(username, sep);
        if(pos)
        {
            *pos = '\0';
            value = pos + 1;
        }
        else
            value = purple_account_user_split_get_default_value(sw->split);
        dw_window_set_text(sw->entry, value ? value : "");
    }
    dw_window_set_text(e->username, username);
    g_free(username);

    /* ---- password / alias / mail / register */
    no_password = info && (info->options & OPT_PROTO_NO_PASSWORD);
    if(!no_password)
    {
        const char *pass = e->account ? purple_account_get_password(e->account) : e->saved_pass;

        e->password = dw_entryfield_password_new(pass ? pass : "", 0);
        dwp_form_row(e->basic_box, (info && (info->options & OPT_PROTO_PASSWORD_OPTIONAL))
                                   ? "Password (optional):" : "Password:", e->password);
        e->remember = dw_checkbox_new("Remember password", 0);
        dw_checkbox_set(e->remember, e->account ? purple_account_get_remember_password(e->account)
                                                : e->saved_remember);
        dwp_form_row(e->basic_box, "", e->remember);
    }
    e->alias = dw_entryfield_new(e->account ? (purple_account_get_alias(e->account)
                                               ? purple_account_get_alias(e->account) : "")
                                            : (e->saved_alias ? e->saved_alias : ""), 0);
    dwp_form_row(e->basic_box, "Local alias:", e->alias);
    if(info && (info->options & OPT_PROTO_MAIL_CHECK))
    {
        e->check_mail = dw_checkbox_new("New mail notifications", 0);
        dw_checkbox_set(e->check_mail, e->account ? purple_account_get_check_mail(e->account) : FALSE);
        dwp_form_row(e->basic_box, "", e->check_mail);
    }
    if(!e->account && info && info->register_user)
    {
        e->reg = dw_checkbox_new("Create this new account on the server", 0);
        dwp_form_row(e->basic_box, "", e->reg);
    }

    /* ---- protocol options */
    for(l = info ? info->protocol_options : NULL; l; l = l->next)
    {
        PurpleAccountOption *opt = l->data;
        const char *setting = purple_account_option_get_setting(opt);
        char *label = dwp_strip_mnemonic(purple_account_option_get_text(opt));
        OptionWidget *o = g_new0(OptionWidget, 1);

        o->option = opt;
        switch(purple_account_option_get_type(opt))
        {
            case PURPLE_PREF_BOOLEAN:
            {
                gboolean def = purple_account_option_get_default_bool(opt);

                o->widget = dw_checkbox_new(label, 0);
                dw_checkbox_set(o->widget, e->account ? purple_account_get_bool(e->account, setting, def) : def);
                dwp_form_row(e->adv_box, "", o->widget);
                break;
            }
            case PURPLE_PREF_INT:
            {
                int def = purple_account_option_get_default_int(opt);
                int value = e->account ? purple_account_get_int(e->account, setting, def) : def;
                char buf[32];

                g_snprintf(buf, sizeof(buf), "%d", value);
                o->widget = dw_spinbutton_new(buf, 0);
                dw_spinbutton_set_limits(o->widget, G_MAXINT, G_MININT);
                dw_spinbutton_set_pos(o->widget, value);
                dwp_form_row(e->adv_box, label, o->widget);
                break;
            }
            case PURPLE_PREF_STRING:
            {
                const char *def = purple_account_option_get_default_string(opt);
                const char *value = e->account ? purple_account_get_string(e->account, setting, def) : def;

                o->widget = purple_account_option_get_masked(opt)
                            ? dw_entryfield_password_new(value ? value : "", 0)
                            : dw_entryfield_new(value ? value : "", 0);
                dwp_form_row(e->adv_box, label, o->widget);
                break;
            }
            case PURPLE_PREF_STRING_LIST:
            {
                const char *def = purple_account_option_get_default_list_value(opt);
                const char *current = e->account ? purple_account_get_string(e->account, setting, def) : def;
                GList *i;
                int sel = -1;

                o->values = g_ptr_array_new_with_free_func(g_free);
                o->labels = g_ptr_array_new_with_free_func(g_free);
                for(i = purple_account_option_get_list(opt); i; i = i->next)
                {
                    PurpleKeyValuePair *kvp = i->data;

                    if(purple_strequal(kvp->value, current))
                        sel = (int)o->values->len;
                    g_ptr_array_add(o->labels, g_strdup(kvp->key));
                    g_ptr_array_add(o->values, g_strdup(kvp->value));
                }
                o->widget = dw_combobox_new("", 0);
                dwp_combo_fill(o->widget, (char **)o->labels->pdata, (int)o->labels->len, sel >= 0 ? sel : 0);
                dwp_form_row(e->adv_box, label, o->widget);
                break;
            }
            default:
                g_free(o);
                o = NULL;
                break;
        }
        if(o)
            g_ptr_array_add(e->options, o);
        g_free(label);
    }
    if(!e->options->len)
        dw_box_pack_start(e->adv_box, dwp_label("This protocol has no additional options."),
                          DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 4);
    dw_window_redraw(e->window);
}

static int DWSIGNAL editor_protocol_changed(HWND w, int index, void *data)
{
    Editor *e = data;

    (void)w;
    if(index == e->last_protocol)
        return FALSE;
    e->last_protocol = index;
    editor_build(e);
    return FALSE;
}

static char *compose_username(Editor *e)
{
    GString *out = g_string_new(NULL);
    char *base = dwp_get_text_trimmed(e->username);
    guint z;

    g_string_append(out, base);
    g_free(base);
    for(z = 0; z < e->splits->len; z++)
    {
        SplitWidget *sw = &g_array_index(e->splits, SplitWidget, z);
        char *value = dwp_get_text_trimmed(sw->entry);

        if(!*value)
        {
            g_free(value);
            value = g_strdup(purple_account_user_split_get_default_value(sw->split));
        }
        g_string_append_c(out, purple_account_user_split_get_separator(sw->split));
        g_string_append(out, value ? value : "");
        g_free(value);
    }
    return g_string_free(out, FALSE);
}

static int DWSIGNAL editor_cancel(HWND w, void *data)
{
    Editor *e = data;

    (void)w;
    dwp_window_close_later(e->window, editor_free, e);
    return TRUE;
}

static int DWSIGNAL editor_save(HWND w, void *data)
{
    Editor *e = data;
    PurplePlugin *prpl;
    PurplePluginProtocolInfo *info;
    PurpleAccount *account, *existing;
    char *base, *full, *alias;
    const char *proto_id;
    gboolean is_new = (e->account == NULL);
    guint z;

    (void)w;
    if(e->account && !dwp_account_exists(e->account))
    {
        dwp_window_close_later(e->window, editor_free, e);
        return TRUE;
    }
    prpl = editor_prpl(e);
    if(!prpl)
    {
        dw_messagebox("Account", DW_MB_OK | DW_MB_WARNING, "The protocol plugin for this account is not available.");
        return TRUE;
    }
    info = PURPLE_PLUGIN_PROTOCOL_INFO(prpl);
    base = dwp_get_text_trimmed(e->username);
    if(!*base)
    {
        g_free(base);
        dw_messagebox("Account", DW_MB_OK | DW_MB_WARNING, "Please enter a username.");
        return TRUE;
    }
    g_free(base);
    full = compose_username(e);
    proto_id = prpl->info->id;
    existing = purple_accounts_find(full, proto_id);
    if(existing && existing != e->account)
    {
        dw_messagebox("Account", DW_MB_OK | DW_MB_WARNING,
                      "An account named %s already exists for this protocol.", full);
        g_free(full);
        return TRUE;
    }

    account = is_new ? purple_account_new(full, proto_id) : e->account;
    if(!is_new && !purple_strequal(full, purple_account_get_username(account)))
        purple_account_set_username(account, full);
    g_free(full);
    alias = dwp_get_text_trimmed(e->alias);
    purple_account_set_alias(account, *alias ? alias : NULL);
    g_free(alias);
    if(!(info->options & OPT_PROTO_NO_PASSWORD) && e->password)
    {
        char *pass = dwp_get_text(e->password);

        purple_account_set_remember_password(account, dw_checkbox_get(e->remember) ? TRUE : FALSE);
        purple_account_set_password(account, *pass ? pass : NULL);
        g_free(pass);
    }
    if((info->options & OPT_PROTO_MAIL_CHECK) && e->check_mail)
        purple_account_set_check_mail(account, dw_checkbox_get(e->check_mail) ? TRUE : FALSE);

    for(z = 0; z < e->options->len; z++)
    {
        OptionWidget *o = g_ptr_array_index(e->options, z);
        const char *setting = purple_account_option_get_setting(o->option);

        switch(purple_account_option_get_type(o->option))
        {
            case PURPLE_PREF_BOOLEAN:
                purple_account_set_bool(account, setting, dw_checkbox_get(o->widget) ? TRUE : FALSE);
                break;
            case PURPLE_PREF_INT:
                purple_account_set_int(account, setting, (int)dw_spinbutton_get_pos(o->widget));
                break;
            case PURPLE_PREF_STRING:
            {
                char *v = dwp_get_text(o->widget);

                purple_account_set_string(account, setting, v);
                g_free(v);
                break;
            }
            case PURPLE_PREF_STRING_LIST:
            {
                int index = dwp_combo_index(o->widget, (char **)o->labels->pdata, (int)o->labels->len);

                if(index >= 0)
                    purple_account_set_string(account, setting, g_ptr_array_index(o->values, index));
                break;
            }
            default:
                break;
        }
    }

    if(is_new)
    {
        purple_accounts_add(account);
        if(e->reg && dw_checkbox_get(e->reg))
            purple_account_register(account);
        else
        {
            PurpleSavedStatus *saved = purple_savedstatus_get_current();

            if(saved)
            {
                purple_savedstatus_activate_for_account(saved, account);
                purple_account_set_enabled(account, DWP_UI_ID, TRUE);
            }
        }
    }
    dwp_window_close_later(e->window, editor_free, e);
    return TRUE;
}

static gint prpl_compare(gconstpointer a, gconstpointer b)
{
    const PurplePlugin *pa = *(PurplePlugin *const *)a;
    const PurplePlugin *pb = *(PurplePlugin *const *)b;

    return g_utf8_collate(pa->info->name ? pa->info->name : "", pb->info->name ? pb->info->name : "");
}

void dwp_account_edit(PurpleAccount *account)
{
    Editor *e = g_new0(Editor, 1);
    HWND box, page, buttons, scroll;
    unsigned long pageid;
    GPtrArray *prpls = g_ptr_array_new();
    GList *l;
    guint z;

    e->account = account;
    e->prpl_ids = g_ptr_array_new_with_free_func(g_free);
    e->prpl_names = g_ptr_array_new_with_free_func(g_free);
    e->splits = g_array_new(FALSE, TRUE, sizeof(SplitWidget));
    e->options = g_ptr_array_new_with_free_func(option_widget_free);
    e->last_protocol = -1;

    for(l = purple_plugins_get_protocols(); l; l = l->next)
        g_ptr_array_add(prpls, l->data);
    g_ptr_array_sort(prpls, prpl_compare);
    for(z = 0; z < prpls->len; z++)
    {
        PurplePlugin *p = g_ptr_array_index(prpls, z);

        g_ptr_array_add(e->prpl_ids, g_strdup(p->info->id));
        g_ptr_array_add(e->prpl_names, g_strdup(p->info->name ? p->info->name : p->info->id));
    }
    g_ptr_array_free(prpls, TRUE);

    e->window = dwp_window_new(account ? "Modify Account" : "Add Account", 0, 0);
    box = dw_box_new(DW_VERT, 4);
    dw_box_pack_start(e->window, box, 0, 0, TRUE, TRUE, 0);
    if(account)
        dwp_form_row(box, "Protocol:", dwp_label(purple_account_get_protocol_name(account)));
    else
    {
        int sel = 0;

        for(z = 0; z < e->prpl_ids->len; z++)
            if(purple_strequal(g_ptr_array_index(e->prpl_ids, z), "prpl-jabber"))
                sel = (int)z;
        e->protocol = dw_combobox_new("", 0);
        dwp_combo_fill(e->protocol, (char **)e->prpl_names->pdata, (int)e->prpl_names->len, sel);
        e->last_protocol = sel;
        dwp_form_row(box, "Protocol:", e->protocol);
        dw_signal_connect(e->protocol, DW_SIGNAL_LIST_SELECT, DW_SIGNAL_FUNC(editor_protocol_changed), e);
    }

    e->notebook = dw_notebook_new(0, TRUE);
    dw_box_pack_start(box, e->notebook, 480, 320, TRUE, TRUE, 2);

    page = dw_box_new(DW_VERT, 4);
    pageid = dw_notebook_page_new(e->notebook, 0, FALSE);
    dw_notebook_pack(e->notebook, pageid, page);
    dw_notebook_page_set_text(e->notebook, pageid, "Basic");
    e->basic_holder = page;

    page = dw_box_new(DW_VERT, 4);
    scroll = dw_scrollbox_new(DW_VERT, 2);
    dw_box_pack_start(page, scroll, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, TRUE, 0);
    pageid = dw_notebook_page_new(e->notebook, 0, FALSE);
    dw_notebook_pack(e->notebook, pageid, page);
    dw_notebook_page_set_text(e->notebook, pageid, "Advanced");
    e->adv_holder = scroll;

    buttons = dw_box_new(DW_HORZ, 0);
    dw_box_pack_start(box, buttons, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 2);
    dwp_spacer(buttons);
    dwp_button(buttons, account ? "Save" : "Add", DW_SIGNAL_FUNC(editor_save), e);
    dwp_button(buttons, "Cancel", DW_SIGNAL_FUNC(editor_cancel), e);
    dw_signal_connect(e->window, DW_SIGNAL_DELETE, DW_SIGNAL_FUNC(editor_cancel), e);

    editor_build(e);
    dwp_window_present(e->window, 520, 470);
    if(e->username)
        dw_window_set_focus(e->username);
}
