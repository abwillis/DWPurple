/*
 * DWPurple - privacy settings and block list (PurplePrivacyUiOps).
 */
#include "dwpurple.h"

typedef struct {
    HWND window, type, list, list_label;
    DwpAccountCombo *account;
    gboolean refresh_pending, syncing;
} PrivacyWin;

static PrivacyWin *pw = NULL;

static const PurplePrivacyType types[] = {
    PURPLE_PRIVACY_ALLOW_ALL, PURPLE_PRIVACY_ALLOW_BUDDYLIST, PURPLE_PRIVACY_ALLOW_USERS,
    PURPLE_PRIVACY_DENY_USERS, PURPLE_PRIVACY_DENY_ALL
};
static char *type_labels[] = {
    "Allow all users to contact me",
    "Allow only the users on my buddy list",
    "Allow only the users below",
    "Block only the users below",
    "Block all users"
};
#define N_TYPES ((int)(sizeof(types) / sizeof(types[0])))

gboolean dwp_privacy_is_blocked(PurpleAccount *account, const char *who)
{
    return account && who && !purple_privacy_check(account, who);
}

void dwp_privacy_set_blocked(PurpleAccount *account, const char *who, gboolean block)
{
    if(!dwp_account_exists(account) || !who || !*who)
        return;
    if(block)
        purple_privacy_deny(account, who, FALSE, FALSE);
    else
        purple_privacy_allow(account, who, FALSE, FALSE);
}

static gboolean showing_permit(PurpleAccount *account)
{
    return purple_account_get_privacy_type(account) == PURPLE_PRIVACY_ALLOW_USERS;
}

static void refresh(gpointer unused)
{
    PurpleAccount *account;
    PurplePrivacyType type;
    GSList *names, *l;
    int z;

    (void)unused;
    if(!pw)
        return;
    pw->refresh_pending = FALSE;
    account = dwp_account_combo_get(pw->account);
    dw_listbox_clear(pw->list);
    if(!account)
    {
        dw_window_disable(pw->type);
        dw_window_set_text(pw->list_label, "No account selected.");
        return;
    }
    dw_window_enable(pw->type);
    type = purple_account_get_privacy_type(account);
    pw->syncing = TRUE;
    for(z = 0; z < N_TYPES; z++)
        if(types[z] == type)
        {
            dw_listbox_select(pw->type, z, TRUE);
            dw_window_set_text(pw->type, type_labels[z]);
        }
    pw->syncing = FALSE;
    names = showing_permit(account) ? account->permit : account->deny;
    dw_window_set_text(pw->list_label, showing_permit(account) ? "Allowed users:" : "Blocked users:");
    for(l = names; l; l = l->next)
        dw_listbox_append(pw->list, l->data);
}

static void schedule_refresh(void)
{
    if(!pw || pw->refresh_pending)
        return;
    pw->refresh_pending = TRUE;
    dwp_defer(refresh, NULL);
}

static int DWSIGNAL type_selected(HWND w, int index, void *data)
{
    PurpleAccount *account;

    (void)w; (void)data;
    if(!pw || pw->syncing || index < 0 || index >= N_TYPES)
        return FALSE;
    account = dwp_account_combo_get(pw->account);
    if(!account)
        return FALSE;
    purple_account_set_privacy_type(account, types[index]);
    if(purple_account_is_connected(account))
        serv_set_permit_deny(purple_account_get_connection(account));
    schedule_refresh();
    return FALSE;
}

static void add_done(gpointer data, const char *text)
{
    char *name = g_strstrip(g_strdup(text ? text : ""));
    PurpleAccount *account = pw ? dwp_account_combo_get(pw->account) : NULL;

    (void)data;
    if(account && *name)
    {
        if(showing_permit(account))
            purple_privacy_permit_add(account, name, FALSE);
        else
            purple_privacy_deny_add(account, name, FALSE);
    }
    g_free(name);
    schedule_refresh();
}

static int DWSIGNAL add_clicked(HWND w, void *data)
{
    PurpleAccount *account = dwp_account_combo_get(pw->account);

    (void)w; (void)data;
    if(!account)
        return TRUE;
    dwp_prompt(showing_permit(account) ? "Allow User" : "Block User", "Username:", "", FALSE, add_done, NULL);
    return TRUE;
}

static int DWSIGNAL remove_clicked(HWND w, void *data)
{
    PurpleAccount *account = dwp_account_combo_get(pw->account);
    int index = dw_listbox_selected(pw->list);
    char buf[512];

    (void)w; (void)data;
    if(!account || index < 0)
        return TRUE;
    buf[0] = '\0';
    dw_listbox_get_text(pw->list, index, buf, sizeof(buf));
    if(!*buf)
        return TRUE;
    if(showing_permit(account))
        purple_privacy_permit_remove(account, buf, FALSE);
    else
        purple_privacy_deny_remove(account, buf, FALSE);
    schedule_refresh();
    return TRUE;
}

static void account_changed(gpointer data, PurpleAccount *account)
{
    (void)data;
    (void)account;
    schedule_refresh();
}

static void pw_free(gpointer p)
{
    PrivacyWin *d = p;

    dwp_account_combo_free(d->account);
    g_free(d);
}

static int DWSIGNAL close_clicked(HWND w, void *data)
{
    PrivacyWin *d = pw;

    (void)w; (void)data;
    if(!d)
        return TRUE;
    pw = NULL;
    dwp_window_close_later(d->window, pw_free, d);
    return TRUE;
}

void dwp_privacy_show(PurpleAccount *account)
{
    HWND box, buttons;
    int z;

    if(!pw)
    {
        pw = g_new0(PrivacyWin, 1);
        pw->window = dwp_window_new("Privacy", 0, 0);
        box = dw_box_new(DW_VERT, 4);
        dw_box_pack_start(pw->window, box, 0, 0, TRUE, TRUE, 0);
        pw->account = dwp_account_combo_new(FALSE, NULL);
        dwp_form_row(box, "Account:", dwp_account_combo_widget(pw->account));
        pw->type = dw_combobox_new("", 0);
        for(z = 0; z < N_TYPES; z++)
            dw_listbox_append(pw->type, type_labels[z]);
        dw_box_pack_start(box, pw->type, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 2);
        dw_signal_connect(pw->type, DW_SIGNAL_LIST_SELECT, DW_SIGNAL_FUNC(type_selected), NULL);
        pw->list_label = dwp_label("");
        dw_box_pack_start(box, pw->list_label, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 2);
        pw->list = dw_listbox_new(0, FALSE);
        dw_box_pack_start(box, pw->list, 360, 220, TRUE, TRUE, 2);
        buttons = dw_box_new(DW_HORZ, 0);
        dw_box_pack_start(box, buttons, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 2);
        dwp_button(buttons, "Add...", DW_SIGNAL_FUNC(add_clicked), NULL);
        dwp_button(buttons, "Remove", DW_SIGNAL_FUNC(remove_clicked), NULL);
        dwp_spacer(buttons);
        dwp_button(buttons, "Close", DW_SIGNAL_FUNC(close_clicked), NULL);
        dw_signal_connect(pw->window, DW_SIGNAL_DELETE, DW_SIGNAL_FUNC(close_clicked), NULL);
        dwp_account_combo_on_change(pw->account, account_changed, NULL);
        dwp_window_present(pw->window, 440, 400);
    }
    else
    {
        dw_window_show(pw->window);
        dw_window_raise(pw->window);
    }
    if(account)
        dwp_account_combo_set(pw->account, account);
    refresh(NULL);
}

/* ================================================================ ui ops */

static void list_changed(PurpleAccount *account, const char *name)
{
    (void)account;
    (void)name;
    schedule_refresh();
}

PurplePrivacyUiOps *dwp_privacy_ui_ops(void)
{
    static PurplePrivacyUiOps ops;
    static gboolean ready = FALSE;

    if(!ready)
    {
        memset(&ops, 0, sizeof(ops));
        ops.permit_added = list_changed;
        ops.permit_removed = list_changed;
        ops.deny_added = list_changed;
        ops.deny_removed = list_changed;
        ready = TRUE;
    }
    return &ops;
}
