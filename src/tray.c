/*
 * DWPurple - system tray icon.
 *
 * Dynamic Windows reports tray clicks as DW_SIGNAL_BUTTON_PRESS on the
 * widget given to dw_taskbar_insert(); the buddy list owns a 1x1 render
 * widget for that (see dwp_blist_tray_anchor()).  On OS/2 the tray needs
 * a SysTray/xCenter tray server; without one the calls do nothing.
 */
#include "dwpurple.h"

static gboolean shown = FALSE;
static HICN current_icon = 0;
static char *current_tip = NULL;

gboolean dwp_tray_shown(void)
{
    return shown;
}

static HICN pick_icon(int *unread)
{
    PurpleSavedStatus *saved = purple_savedstatus_get_current();
    PurpleStatusPrimitive prim = saved ? purple_savedstatus_get_type(saved) : PURPLE_STATUS_AVAILABLE;
    gboolean any_online = FALSE;
    GList *l;

    *unread = dwp_conv_unread_count();
    for(l = purple_accounts_get_all_active(); l; l = g_list_delete_link(l, l))
        if(purple_account_is_connected(l->data))
            any_online = TRUE;
    if(*unread && dwp_chat_icon())
        return dwp_chat_icon();
    if(!any_online)
        prim = PURPLE_STATUS_OFFLINE;
    return dwp_status_icon(prim, FALSE) ? dwp_status_icon(prim, FALSE) : dwp_app_icon();
}

void dwp_tray_update(void)
{
    HWND anchor = dwp_blist_tray_anchor();
    int unread = 0;
    HICN icon;
    char *tip;
    PurpleSavedStatus *saved;

    if(!shown || !anchor)
        return;
    icon = pick_icon(&unread);
    saved = purple_savedstatus_get_current();
    if(unread)
        tip = g_strdup_printf("%s - %d unread conversation%s", DWP_NAME, unread, unread == 1 ? "" : "s");
    else
        tip = g_strdup_printf("%s - %s", DWP_NAME,
                              dwp_primitive_label(saved ? purple_savedstatus_get_type(saved) : PURPLE_STATUS_AVAILABLE));
    if(icon == current_icon && purple_strequal(tip, current_tip))
    {
        g_free(tip);
        return;
    }
    if(current_icon)
        dw_taskbar_delete(anchor, current_icon);
    if(icon)
        dw_taskbar_insert(anchor, icon, tip);
    current_icon = icon;
    g_free(current_tip);
    current_tip = tip;
}

static void tray_remove(void)
{
    HWND anchor = dwp_blist_tray_anchor();

    if(shown && anchor && current_icon)
        dw_taskbar_delete(anchor, current_icon);
    shown = FALSE;
    current_icon = 0;
    g_free(current_tip);
    current_tip = NULL;
}

void dwp_tray_apply_prefs(void)
{
    gboolean want = purple_prefs_get_bool(DWP_PREF_TRAY_ENABLED) &&
                    dw_feature_get(DW_FEATURE_TASK_BAR) != DW_FEATURE_UNSUPPORTED &&
                    dwp_app_icon() != 0;

    if(want && !shown)
    {
        shown = TRUE;
        dwp_tray_update();
    }
    else if(!want && shown)
    {
        tray_remove();
        /* Without a tray the buddy list must stay reachable. */
        dwp_blist_present();
    }
}

void dwp_tray_shutdown(void)
{
    tray_remove();
}

/* ------------------------------------------------------------------ menu */

static void tm_toggle(gpointer a, gpointer b, const char *s) { (void)a; (void)b; (void)s; dwp_blist_toggle(); }
static void tm_accounts(gpointer a, gpointer b, const char *s) { (void)a; (void)b; (void)s; dwp_accounts_show(); }
static void tm_prefs(gpointer a, gpointer b, const char *s) { (void)a; (void)b; (void)s; dwp_prefs_show(); }
static void tm_new_im(gpointer a, gpointer b, const char *s) { (void)a; (void)b; (void)s; dwp_show_new_im(); }
static void tm_join(gpointer a, gpointer b, const char *s) { (void)a; (void)b; (void)s; dwp_show_join_chat(); }
static void tm_quit(gpointer a, gpointer b, const char *s) { (void)a; (void)b; (void)s; dwp_core_request_quit(); }

static void tm_mute(gpointer a, gpointer b, const char *s)
{
    (void)a; (void)b; (void)s;
    dwp_sound_set_muted(!dwp_sound_muted());
    dwp_blist_sync_mute();
}

static void tm_status(gpointer prim, gpointer b, const char *s)
{
    (void)b; (void)s;
    dwp_blist_set_status_primitive((PurpleStatusPrimitive)GPOINTER_TO_INT(prim));
}

static void tm_unread(gpointer a, gpointer b, const char *s)
{
    (void)a; (void)b; (void)s;
    dwp_conv_present_first_unread();
}

static void tray_menu(void)
{
    static const PurpleStatusPrimitive prims[] = {
        PURPLE_STATUS_AVAILABLE, PURPLE_STATUS_AWAY, PURPLE_STATUS_UNAVAILABLE,
        PURPLE_STATUS_EXTENDED_AWAY, PURPLE_STATUS_INVISIBLE, PURPLE_STATUS_OFFLINE
    };
    HMENUI menu, status;
    size_t z;

    dwp_actions_reset();
    menu = dw_menu_new(0);
    dwp_menu_add(menu, dwp_blist_visible() ? "Hide ~Buddy List" : "Show ~Buddy List", tm_toggle, NULL, NULL, NULL);
    if(dwp_conv_unread_count())
        dwp_menu_add(menu, "Show ~Unread Messages", tm_unread, NULL, NULL, NULL);
    dwp_menu_separator(menu);
    status = dw_menu_new(0);
    for(z = 0; z < sizeof(prims) / sizeof(prims[0]); z++)
        dwp_menu_add(status, dwp_primitive_label(prims[z]), tm_status, GINT_TO_POINTER(prims[z]), NULL, NULL);
    dw_menu_append_item(menu, "Change ~Status", DW_MENU_POPUP, 0, TRUE, FALSE, status);
    dwp_menu_add(menu, "New ~Message...", tm_new_im, NULL, NULL, NULL);
    dwp_menu_add(menu, "~Join a Chat...", tm_join, NULL, NULL, NULL);
    dwp_menu_separator(menu);
    dwp_menu_add(menu, "~Accounts", tm_accounts, NULL, NULL, NULL);
    dwp_menu_add(menu, "Pr~eferences", tm_prefs, NULL, NULL, NULL);
    dwp_menu_add(menu, dwp_sound_muted() ? "Un~mute Sounds" : "~Mute Sounds", tm_mute, NULL, NULL, NULL);
    dwp_menu_separator(menu);
    dwp_menu_add(menu, "~Quit", tm_quit, NULL, NULL, NULL);
    dwp_menu_popup_here(&menu, dwp_blist_window());
}

void dwp_tray_clicked(int button)
{
    dwp_note_activity();
    if(button & DW_BUTTON1_MASK)
    {
        /* Left click: show unread messages first, else toggle the list. */
        if(!dwp_conv_present_first_unread())
            dwp_blist_toggle();
    }
    else
        tray_menu();
}
