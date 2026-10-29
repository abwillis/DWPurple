/*
 * DWPurple - a Dynamic Windows front end for libpurple
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Common definitions shared by all modules.
 *
 * Portability notes:
 *  - HWND/HMENUI/HICN are integers on OS/2 and pointers elsewhere, so they
 *    are only ever compared with 0 / DW_NOHWND and never stored as void *.
 *  - Every Dynamic Windows callback is declared with DWSIGNAL (the OS/2
 *    calling convention differs from the C default).
 */
#ifndef DWPURPLE_H
#define DWPURPLE_H

/* dw.h first: some ports define TRUE/FALSE unconditionally, glib only
 * when they are not yet defined. */
#include <dw.h>
#include <purple.h>

#include <string.h>
#include <stdlib.h>
#include <time.h>

#define DWP_UI_ID           "dwpurple"
#define DWP_NAME            "DWPurple"
#define DWP_VERSION         "0.1.0"
#define DWP_WEBSITE         "https://github.com/"
#define DWP_APP_ID          "org.dwpurple.dwpurple"

/* ------------------------------------------------------------ preferences */
#define DWP_PREFS_ROOT          "/dwpurple"
#define DWP_PREF_PLUGINS        "/dwpurple/plugins"
#define DWP_PREF_LOADED         "/dwpurple/plugins/loaded"
#define DWP_PREF_BLIST          "/dwpurple/blist"
#define DWP_PREF_OFFLINE        "/dwpurple/blist/show_offline_buddies"
#define DWP_PREF_EMPTY          "/dwpurple/blist/show_empty_groups"
#define DWP_PREF_BLIST_W        "/dwpurple/blist/width"
#define DWP_PREF_BLIST_H        "/dwpurple/blist/height"
#define DWP_PREF_CONV           "/dwpurple/conversations"
#define DWP_PREF_CONV_W         "/dwpurple/conversations/width"
#define DWP_PREF_CONV_H         "/dwpurple/conversations/height"
#define DWP_PREF_TIMESTAMPS     "/dwpurple/conversations/show_timestamps"
#define DWP_PREF_SHOW_REACTIONS "/dwpurple/conversations/show_reactions"
#define DWP_PREF_EMOJI_CODES    "/dwpurple/conversations/emoji_codes"
#define DWP_PREF_NOTIFY_MSGS    "/dwpurple/conversations/system_notifications"
#define DWP_PREF_RAISE_NEW      "/dwpurple/conversations/raise_new"
#define DWP_PREF_SOUND          "/dwpurple/sound"
#define DWP_PREF_SOUND_ENABLED  "/dwpurple/sound/enabled"
#define DWP_PREF_SOUND_FILE     "/dwpurple/sound/file"
#define DWP_PREF_SOUND_METHOD   "/dwpurple/sound/method"
#define DWP_PREF_SOUND_COMMAND  "/dwpurple/sound/command"
#define DWP_PREF_SOUND_MUTE     "/dwpurple/sound/mute"
#define DWP_PREF_SOUND_FOCUS    "/dwpurple/sound/conv_focus"
#define DWP_PREF_TRAY           "/dwpurple/tray"
#define DWP_PREF_TRAY_ENABLED   "/dwpurple/tray/enabled"

/* Fixed menu item ids (dynamic and popup items use DW_MENU_AUTO/POPUP). */
enum {
    DWP_MID_BASE = 1100,
    DWP_MID_SHOW_OFFLINE,
    DWP_MID_SHOW_EMPTY,
    DWP_MID_MUTE,
    DWP_MID_CONV_REACTIONS,
    DWP_MID_CONV_EMOJI,
    DWP_MID_CONV_TIMESTAMPS
};

/* ---------------------------------------------------------------- helpers */

/* Copy of a Dynamic Windows string (frees the dw allocation); never NULL. */
char *dwp_get_text(HWND handle);
/* Same, trimmed of leading/trailing white space. */
char *dwp_get_text_trimmed(HWND handle);

/* Text of an MLE (multi-line edit), portable across ports; never NULL. */
char *dwp_mle_get_text(HWND mle);
/* Replace the MLE text; returns the insertion point after it. */
unsigned int dwp_mle_set_text(HWND mle, const char *text);

gboolean dwp_account_exists(PurpleAccount *account);
gboolean dwp_connection_exists(PurpleConnection *gc);
gboolean dwp_conversation_exists(PurpleConversation *conv);
gboolean dwp_blist_node_exists(PurpleBlistNode *node);
PurplePluginProtocolInfo *dwp_prpl_info(PurpleConnection *gc);
PurplePluginProtocolInfo *dwp_account_prpl_info(PurpleAccount *account, PurpleConnection **gc_out);

/* "user (Protocol)", newly allocated. */
char *dwp_account_display_name(PurpleAccount *account);
const char *dwp_primitive_label(PurpleStatusPrimitive prim);
PurpleStatusPrimitive dwp_account_primitive(PurpleAccount *account);
PurpleStatusPrimitive dwp_buddy_primitive(PurpleBuddy *buddy, gboolean *idle);

/* GTK style "_Mnemonic" -> Dynamic Windows "~Mnemonic". */
char *dwp_mnemonic(const char *text);
/* Remove GTK style mnemonics. */
char *dwp_strip_mnemonic(const char *text);

/* Open a link from untrusted content (web/mail/xmpp/irc only). */
gboolean dwp_open_link(const char *url);

/* Local file name (glib encoding) <-> UTF-8 for display. */
char *dwp_filename_to_utf8(const char *name);

/* Remember where the executable lives (icon lookup). */
void dwp_set_exe_path(const char *argv0);
/* Status icon (0 when the icon files are not installed). */
HICN dwp_status_icon(PurpleStatusPrimitive prim, gboolean idle);
HICN dwp_chat_icon(void);
HICN dwp_app_icon(void);
void dwp_icons_free(void);

/* Activity in DWPurple's own windows (fallback idle source). */
void dwp_note_activity(void);
time_t dwp_last_activity(void);

/* ------------------------------------------------------ deferred tasks */
typedef void (*DwpTaskFunc)(gpointer data);
/* Run func(data) soon from the main loop, outside the current callback. */
void dwp_defer(DwpTaskFunc func, gpointer data);
/* Hide now, destroy from the main loop; free_data(data) afterwards. */
void dwp_window_close_later(HWND window, GDestroyNotify free_data, gpointer data);
void dwp_tasks_start(void);
void dwp_tasks_stop(void);

/* ---------------------------------------------------- popup menu items */
typedef void (*DwpActionFunc)(gpointer a, gpointer b, const char *str);
/* Discard all actions created for previous popup menus. */
void dwp_actions_reset(void);
/* Append a popup item calling func(a, b, str); str is copied. */
HWND dwp_menu_add(HMENUI menu, const char *title, DwpActionFunc func,
                  gpointer a, gpointer b, const char *str);
/* Same for a menu that lives as long as the program (menu bars). */
HWND dwp_menu_add_static(HMENUI menu, const char *title, unsigned long id, int check,
                         DwpActionFunc func, gpointer a);
void dwp_menu_separator(HMENUI menu);
/* Check/uncheck a menu item without running its click handler (GTK emits
 * "activate" when a check item changes state, which would re-run the
 * toggle action and recurse). */
void dwp_menu_set_state(HMENUI menu, unsigned long id, unsigned long state);
/* Pop a menu up at the mouse pointer. */
void dwp_menu_popup_here(HMENUI *menu, HWND parent);

/* --------------------------------------------------------- layout helpers */
HWND dwp_window_new(const char *title, int width, int height);
HWND dwp_label(const char *text);
HWND dwp_form_row(HWND box, const char *label, HWND widget);
HWND dwp_button(HWND box, const char *text, void *func, void *data);
void dwp_spacer(HWND box);
/* Pack read-only, selectable, word-wrapped text sized to its content
 * (capped at max_height; 0 = no cap).  Used instead of word-wrapped
 * dw_text_new() labels, which GTK 3 can size so tall that the whole
 * window lays out blank. */
HWND dwp_text_block(HWND box, const char *text, int width, int max_height);
/* Remove and destroy old (if any) and pack a fresh box into holder. */
HWND dwp_box_reset(HWND holder, HWND old, int type);
/* Show a window centred on the screen. */
void dwp_window_present(HWND window, int width, int height);

/* Combo helpers: pick by index, falling back to matching the typed text. */
int dwp_combo_index(HWND combo, char **items, int count);
void dwp_combo_fill(HWND combo, char **items, int count, int selected);

/* Plain text prompt (non-modal). */
typedef void (*DwpPromptFunc)(gpointer data, const char *text);
void dwp_prompt(const char *title, const char *label, const char *initial, gboolean masked,
                DwpPromptFunc ok, gpointer data);
/* Simple non-modal message window. */
void dwp_message(const char *title, const char *primary, const char *secondary);
gboolean dwp_confirm(const char *title, const char *fmt, ...) G_GNUC_PRINTF(2, 3);

/* Combo box of accounts, kept up to date. */
typedef gboolean (*DwpAccountFilter)(PurpleAccount *account);
typedef struct _DwpAccountCombo DwpAccountCombo;
typedef void (*DwpAccountChanged)(gpointer data, PurpleAccount *account);
DwpAccountCombo *dwp_account_combo_new(gboolean online_only, DwpAccountFilter filter);
HWND dwp_account_combo_widget(DwpAccountCombo *ac);
PurpleAccount *dwp_account_combo_get(DwpAccountCombo *ac);
void dwp_account_combo_set(DwpAccountCombo *ac, PurpleAccount *account);
void dwp_account_combo_refill(DwpAccountCombo *ac);
void dwp_account_combo_on_change(DwpAccountCombo *ac, DwpAccountChanged cb, gpointer data);
void dwp_account_combo_free(DwpAccountCombo *ac);

/* Group combo (editable, lists all groups). */
HWND dwp_group_combo_new(const char *initial);
PurpleGroup *dwp_group_find_or_create(const char *name);

/* GDK 3 lock watchdog; call before dw_init() (gdklock.c). */
void dwp_gdk_lock_install(void);
/* Hold the GDK thread lock around Dynamic Windows timer callbacks (GTK 3
 * expects it held in callbacks; no-op elsewhere). */
void dwp_gui_lock(void);
void dwp_gui_unlock(void);

/* ------------------------------------------------------------------ core */
typedef struct {
    char *config_dir;
    gboolean debug;
    gboolean no_login;
} DwpOptions;

gboolean dwp_core_init(const DwpOptions *options, char **error);
void dwp_core_request_quit(void);
void dwp_core_shutdown(void);
gboolean dwp_core_shutting_down(void);
void dwp_core_reconnect_now(PurpleAccount *account);

/* ------------------------------------------------------------- eventloop */
PurpleEventLoopUiOps *dwp_eventloop_ui_ops(void);
void dwp_eventloop_start(void);
void dwp_eventloop_shutdown(void);

/* ---------------------------------------------------------------- modules */
PurpleBlistUiOps *dwp_blist_ui_ops(void);
void dwp_blist_create(void);
void dwp_blist_apply_prefs(void);
void dwp_blist_present(void);
void dwp_blist_toggle(void);
gboolean dwp_blist_visible(void);
void dwp_blist_status_message(const char *text);
void dwp_blist_set_status_primitive(PurpleStatusPrimitive prim);
void dwp_blist_sync_mute(void);
void dwp_blist_save_geometry(void);
HWND dwp_blist_window(void);
HWND dwp_blist_tray_anchor(void);
void dwp_show_add_buddy(PurpleAccount *account, const char *user, const char *group, const char *alias);
void dwp_show_add_chat(PurpleAccount *account, const char *group, const char *alias, const char *name);
void dwp_show_join_chat(void);
void dwp_show_add_group(void);
void dwp_show_new_im(void);
void dwp_show_get_info(void);
void dwp_open_im(PurpleAccount *account, const char *name);

void dwp_accounts_show(void);
/* IRC server console (ircconsole.c). */
void dwp_irc_console_init(void);
void dwp_irc_console_show(PurpleAccount *account);
gboolean dwp_irc_console_supported(PurpleAccount *account);
void dwp_account_edit(PurpleAccount *account);
void dwp_account_actions_menu(PurpleAccount *account, HWND parent);
/* Popup with a submenu of actions for every account. */
void dwp_accounts_popup(HWND parent);

PurpleConversationUiOps *dwp_conv_ui_ops(void);
void dwp_conv_init(void);
int dwp_conv_unread_count(void);
gboolean dwp_conv_present_first_unread(void);
void dwp_conv_save_geometry(void);

PurpleRequestUiOps *dwp_request_ui_ops(void);
PurpleNotifyUiOps *dwp_notify_ui_ops(void);
PurpleXferUiOps *dwp_xfer_ui_ops(void);
void dwp_xfers_show(void);
void dwp_plugins_show(void);
PurpleDebugUiOps *dwp_debug_ui_ops(void);
void dwp_debug_show(void);

PurpleSoundUiOps *dwp_sound_ui_ops(void);
void dwp_sound_add_prefs(void);
gboolean dwp_sound_muted(void);
void dwp_sound_set_muted(gboolean mute);
typedef struct {
    PurpleSoundEventID id;
    const char *pref;
    const char *label;
    const char *default_file;
    gboolean default_enabled;
} DwpSoundEvent;
const DwpSoundEvent *dwp_sound_events(int *count);
char *dwp_sound_enabled_pref(const DwpSoundEvent *e);
char *dwp_sound_file_pref(const DwpSoundEvent *e);
char *dwp_sound_default_file(const DwpSoundEvent *e);
char *dwp_sound_effective_file(const DwpSoundEvent *e);
void dwp_sound_test(const char *file);

PurpleIdleUiOps *dwp_idle_ui_ops(void);
void dwp_idle_shutdown(void);
const char *dwp_idle_backend_name(void);

void dwp_pounces_init(void);
void dwp_pounces_show(void);
void dwp_pounce_edit(PurplePounce *pounce, PurpleAccount *account, const char *buddy);

void dwp_prefs_show(void);

PurpleRoomlistUiOps *dwp_roomlist_ui_ops(void);
void dwp_roomlist_show(PurpleAccount *account, gboolean fetch);

PurplePrivacyUiOps *dwp_privacy_ui_ops(void);
void dwp_privacy_show(PurpleAccount *account);
gboolean dwp_privacy_is_blocked(PurpleAccount *account, const char *who);
void dwp_privacy_set_blocked(PurpleAccount *account, const char *who, gboolean block);

void dwp_tray_apply_prefs(void);
gboolean dwp_tray_shown(void);
void dwp_tray_update(void);
void dwp_tray_shutdown(void);
void dwp_tray_clicked(int button);

/* emoji.c */
void dwp_emoji_add_prefs(void);
/* Message HTML -> display text; img tags become their alt/title text. */
char *dwp_html_to_text(const char *html, gboolean emoji_codes);
gboolean dwp_is_reaction(const char *plain);
char *dwp_emoji_user_map_path(void);
/* Picker entries: NULL terminated array of ":code:" strings. */
const char *const *dwp_emoji_picker_codes(void);
const char *dwp_emoji_lookup(const char *code);

#endif
