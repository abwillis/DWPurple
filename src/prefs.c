/*
 * DWPurple - preferences dialog.
 * Changes are applied with Save; Close discards them.
 */
#include "dwpurple.h"

typedef struct {
    HWND enabled, file;
    const DwpSoundEvent *event;
} SoundRow;

typedef struct {
    HWND window, notebook;
    /* interface */
    HWND show_offline, show_empty, tray, raise_new, notify_msgs;
    /* conversations */
    HWND timestamps, reactions, emoji, send_typing, emoji_file;
    /* sounds */
    HWND sound_method, sound_command, sound_mute, sound_focus, sound_while;
    SoundRow *sounds;
    int nsounds;
    /* status / idle */
    HWND idle_reporting, away_when_idle, mins_before_away, auto_reply;
    /* logging */
    HWND log_ims, log_chats, log_system;
    /* network */
    HWND proxy_type, proxy_host, proxy_port, proxy_user, proxy_pass, stun;
} Prefs;

static Prefs *pd = NULL;

static char *method_labels[] = { "Automatic", "Command", "Beep only", "No sounds" };
static const char *const method_values[] = { "automatic", "command", "beep", "none" };
static char *while_labels[] = { "Only when available", "Only when not available", "Always" };
static char *idle_labels[] = { "Never", "From last sent message", "Based on keyboard or mouse use" };
static const char *const idle_values[] = { "none", "purple", "system" };
static char *reply_labels[] = { "Never", "When away", "When both away and idle" };
static const char *const reply_values[] = { "never", "away", "awayidle" };
static char *proxy_labels[] = { "No proxy", "Use environment settings", "HTTP", "SOCKS 4", "SOCKS 5", "Tor/Privacy (SOCKS5)" };
static const char *const proxy_values[] = { "none", "envvar", "http", "socks4", "socks5", "tor" };

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

static int string_index(const char *const *values, int count, const char *value)
{
    int z;

    for(z = 0; z < count; z++)
        if(purple_strequal(values[z], value))
            return z;
    return 0;
}

static HWND check(HWND box, const char *label, gboolean on)
{
    HWND c = dw_checkbox_new(label, 0);

    dw_checkbox_set(c, on);
    dw_box_pack_start(box, c, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 2);
    return c;
}

static HWND combo(HWND box, const char *label, char **items, int count, int sel)
{
    HWND c = dw_combobox_new("", 0);

    dwp_combo_fill(c, items, count, sel);
    dwp_form_row(box, label, c);
    return c;
}

static HWND entry(HWND box, const char *label, const char *value, gboolean masked)
{
    HWND e = masked ? dw_entryfield_password_new(value ? value : "", 0) : dw_entryfield_new(value ? value : "", 0);

    dwp_form_row(box, label, e);
    return e;
}

static HWND spin(HWND box, const char *label, int value, int min, int max)
{
    char buf[32];
    HWND s;

    g_snprintf(buf, sizeof(buf), "%d", value);
    s = dw_spinbutton_new(buf, 0);
    dw_spinbutton_set_limits(s, max, min);
    dw_spinbutton_set_pos(s, value);
    dwp_form_row(box, label, s);
    return s;
}

static HWND new_page(HWND notebook, const char *title, gboolean scroll)
{
    HWND page = dw_box_new(DW_VERT, 4), inner = page;
    unsigned long id = dw_notebook_page_new(notebook, 0, FALSE);

    dw_notebook_pack(notebook, id, page);
    dw_notebook_page_set_text(notebook, id, title);
    if(scroll)
    {
        inner = dw_scrollbox_new(DW_VERT, 2);
        dw_box_pack_start(page, inner, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, TRUE, 0);
    }
    return inner;
}

/* ------------------------------------------------------------ sound rows */

static int DWSIGNAL sound_test(HWND w, void *data)
{
    SoundRow *row = data;
    char *file;

    (void)w;
    if(!pd)
        return TRUE;
    file = dwp_get_text_trimmed(row->file);
    if(!*file)
    {
        g_free(file);
        file = dwp_sound_default_file(row->event);
    }
    dwp_sound_test(file);
    g_free(file);
    return TRUE;
}

static int DWSIGNAL sound_browse(HWND w, void *data)
{
    SoundRow *row = data;
    Prefs *owner = pd;
    char *path;

    (void)w;
    path = dw_file_browse("Choose Sound", NULL, "wav", DW_FILE_OPEN);
    if(path)
    {
        /* The dialog may have been closed during the nested loop. */
        if(pd && pd == owner)
            dw_window_set_text(row->file, path);
        dw_free(path);
    }
    return TRUE;
}

static int DWSIGNAL sound_reset(HWND w, void *data)
{
    SoundRow *row = data;

    (void)w;
    if(pd)
        dw_window_set_text(row->file, "");
    return TRUE;
}

/* ------------------------------------------------------------------ save */

static void prefs_free(gpointer p)
{
    Prefs *d = p;

    g_free(d->sounds);
    g_free(d);
}

static int DWSIGNAL prefs_close(HWND w, void *data)
{
    Prefs *d = pd;

    (void)w;
    (void)data;
    if(!d)
        return TRUE;
    pd = NULL;
    dwp_window_close_later(d->window, prefs_free, d);
    return TRUE;
}

static void set_combo_string(HWND c, char **labels, const char *const *values, int count, const char *pref)
{
    int index = dwp_combo_index(c, labels, count);

    if(index >= 0)
        purple_prefs_set_string(pref, values[index]);
}

static int DWSIGNAL prefs_save(HWND w, void *data)
{
    Prefs *d = pd;
    int z, index;
    char *s;

    (void)data;
    if(!d)
        return TRUE;
    /* Interface */
    purple_prefs_set_bool(DWP_PREF_OFFLINE, dw_checkbox_get(d->show_offline) ? TRUE : FALSE);
    purple_prefs_set_bool(DWP_PREF_EMPTY, dw_checkbox_get(d->show_empty) ? TRUE : FALSE);
    purple_prefs_set_bool(DWP_PREF_TRAY_ENABLED, dw_checkbox_get(d->tray) ? TRUE : FALSE);
    purple_prefs_set_bool(DWP_PREF_RAISE_NEW, dw_checkbox_get(d->raise_new) ? TRUE : FALSE);
    purple_prefs_set_bool(DWP_PREF_NOTIFY_MSGS, dw_checkbox_get(d->notify_msgs) ? TRUE : FALSE);
    /* Conversations */
    purple_prefs_set_bool(DWP_PREF_TIMESTAMPS, dw_checkbox_get(d->timestamps) ? TRUE : FALSE);
    purple_prefs_set_bool(DWP_PREF_SHOW_REACTIONS, dw_checkbox_get(d->reactions) ? TRUE : FALSE);
    purple_prefs_set_bool(DWP_PREF_EMOJI_CODES, dw_checkbox_get(d->emoji) ? TRUE : FALSE);
    purple_prefs_set_bool("/purple/conversations/im/send_typing", dw_checkbox_get(d->send_typing) ? TRUE : FALSE);
    /* Sounds */
    set_combo_string(d->sound_method, method_labels, method_values, COUNT(method_labels), DWP_PREF_SOUND_METHOD);
    s = dwp_get_text_trimmed(d->sound_command);
    purple_prefs_set_string(DWP_PREF_SOUND_COMMAND, s);
    g_free(s);
    dwp_sound_set_muted(dw_checkbox_get(d->sound_mute) ? TRUE : FALSE);
    purple_prefs_set_bool(DWP_PREF_SOUND_FOCUS, dw_checkbox_get(d->sound_focus) ? TRUE : FALSE);
    index = dwp_combo_index(d->sound_while, while_labels, COUNT(while_labels));
    if(index >= 0)
        purple_prefs_set_int("/purple/sound/while_status", index + 1);
    for(z = 0; z < d->nsounds; z++)
    {
        char *en = dwp_sound_enabled_pref(d->sounds[z].event);
        char *fp = dwp_sound_file_pref(d->sounds[z].event);
        char *file = dwp_get_text_trimmed(d->sounds[z].file);

        purple_prefs_set_bool(en, dw_checkbox_get(d->sounds[z].enabled) ? TRUE : FALSE);
        purple_prefs_set_path(fp, file);
        g_free(file);
        g_free(fp);
        g_free(en);
    }
    /* Status / idle */
    set_combo_string(d->idle_reporting, idle_labels, idle_values, COUNT(idle_labels), "/purple/away/idle_reporting");
    purple_prefs_set_bool("/purple/away/away_when_idle", dw_checkbox_get(d->away_when_idle) ? TRUE : FALSE);
    purple_prefs_set_int("/purple/away/mins_before_away", (int)dw_spinbutton_get_pos(d->mins_before_away));
    set_combo_string(d->auto_reply, reply_labels, reply_values, COUNT(reply_labels), "/purple/away/auto_reply");
    /* Logging */
    purple_prefs_set_bool("/purple/logging/log_ims", dw_checkbox_get(d->log_ims) ? TRUE : FALSE);
    purple_prefs_set_bool("/purple/logging/log_chats", dw_checkbox_get(d->log_chats) ? TRUE : FALSE);
    purple_prefs_set_bool("/purple/logging/log_system", dw_checkbox_get(d->log_system) ? TRUE : FALSE);
    /* Network */
    set_combo_string(d->proxy_type, proxy_labels, proxy_values, COUNT(proxy_labels), "/purple/proxy/type");
    s = dwp_get_text_trimmed(d->proxy_host);
    purple_prefs_set_string("/purple/proxy/host", s);
    g_free(s);
    purple_prefs_set_int("/purple/proxy/port", (int)dw_spinbutton_get_pos(d->proxy_port));
    s = dwp_get_text_trimmed(d->proxy_user);
    purple_prefs_set_string("/purple/proxy/username", s);
    g_free(s);
    s = dwp_get_text(d->proxy_pass);
    purple_prefs_set_string("/purple/proxy/password", s);
    g_free(s);
    s = dwp_get_text_trimmed(d->stun);
    purple_prefs_set_string("/purple/network/stun_server", s);
    g_free(s);

    dwp_blist_apply_prefs();
    dwp_tray_apply_prefs();
    return prefs_close(w, NULL);
}

void dwp_prefs_show(void)
{
    HWND box, page, buttons, row, group;
    const DwpSoundEvent *events;
    char *mins;
    int z, count;

    if(pd)
    {
        dw_window_show(pd->window);
        dw_window_raise(pd->window);
        return;
    }
    pd = g_new0(Prefs, 1);
    pd->window = dwp_window_new("Preferences", 0, 0);
    box = dw_box_new(DW_VERT, 4);
    dw_box_pack_start(pd->window, box, 0, 0, TRUE, TRUE, 0);
    pd->notebook = dw_notebook_new(0, TRUE);
    dw_box_pack_start(box, pd->notebook, 560, 420, TRUE, TRUE, 2);

    /* ---- Interface */
    page = new_page(pd->notebook, "Interface", FALSE);
    pd->show_offline = check(page, "Show offline buddies", purple_prefs_get_bool(DWP_PREF_OFFLINE));
    pd->show_empty = check(page, "Show empty groups", purple_prefs_get_bool(DWP_PREF_EMPTY));
    pd->tray = check(page, "Show system tray icon (closing the buddy list hides it)",
                     purple_prefs_get_bool(DWP_PREF_TRAY_ENABLED));
    pd->raise_new = check(page, "Raise the conversation window for new conversations",
                          purple_prefs_get_bool(DWP_PREF_RAISE_NEW));
    pd->notify_msgs = check(page, "Desktop notification for new messages in background tabs",
                            purple_prefs_get_bool(DWP_PREF_NOTIFY_MSGS));
    if(dw_feature_get(DW_FEATURE_NOTIFICATION) != DW_FEATURE_ENABLED)
        dw_window_disable(pd->notify_msgs);
    if(dw_feature_get(DW_FEATURE_TASK_BAR) == DW_FEATURE_UNSUPPORTED)
        dw_window_disable(pd->tray);

    /* ---- Conversations */
    page = new_page(pd->notebook, "Conversations", FALSE);
    pd->timestamps = check(page, "Show timestamps", purple_prefs_get_bool(DWP_PREF_TIMESTAMPS));
    pd->reactions = check(page, "Show reaction notices by default (\"Reacted with ...\")",
                          purple_prefs_get_bool(DWP_PREF_SHOW_REACTIONS));
    pd->emoji = check(page, "Convert :emoji_codes: to emoji by default",
                      purple_prefs_get_bool(DWP_PREF_EMOJI_CODES));
    pd->send_typing = check(page, "Notify buddies that you are typing to them",
                            purple_prefs_get_bool("/purple/conversations/im/send_typing"));
    {
        char *path = dwp_emoji_user_map_path();
        char *text = g_strdup_printf("Custom emoji codes: %s (one \"code = emoji\" per line)", path);
        HWND label = dw_text_new(text, 0);

        dw_window_set_style(label, DW_DT_WORDBREAK, DW_DT_WORDBREAK);
        dw_box_pack_start(page, label, 500, 40, TRUE, FALSE, 4);
        g_free(text);
        g_free(path);
    }
    dw_box_pack_start(page, dwp_label("Per-conversation overrides are in the conversation's Options menu."),
                      DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 4);

    /* ---- Sounds */
    page = new_page(pd->notebook, "Sounds", TRUE);
    pd->sound_method = combo(page, "Method:", method_labels, COUNT(method_labels),
                             string_index(method_values, COUNT(method_values),
                                          purple_prefs_get_string(DWP_PREF_SOUND_METHOD)));
    pd->sound_command = entry(page, "Command (%s = file):", purple_prefs_get_string(DWP_PREF_SOUND_COMMAND), FALSE);
    pd->sound_while = combo(page, "Enable sounds:", while_labels, COUNT(while_labels),
                            CLAMP(purple_prefs_get_int("/purple/sound/while_status"), 1, 3) - 1);
    pd->sound_mute = check(page, "Mute all sounds", dwp_sound_muted());
    pd->sound_focus = check(page, "Play sounds when the conversation has focus",
                            purple_prefs_get_bool(DWP_PREF_SOUND_FOCUS));
    group = dw_groupbox_new(DW_VERT, 2, "Sound Events (empty file = default)");
    dw_box_pack_start(page, group, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 2);
    events = dwp_sound_events(&count);
    pd->sounds = g_new0(SoundRow, count);
    pd->nsounds = count;
    for(z = 0; z < count; z++)
    {
        SoundRow *sr = &pd->sounds[z];
        char *en = dwp_sound_enabled_pref(&events[z]);
        char *fp = dwp_sound_file_pref(&events[z]);
        const char *file = purple_prefs_get_path(fp);

        sr->event = &events[z];
        row = dw_box_new(DW_HORZ, 0);
        dw_box_pack_start(group, row, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 1);
        sr->enabled = dw_checkbox_new(events[z].label, 0);
        dw_checkbox_set(sr->enabled, purple_prefs_get_bool(en));
        dw_box_pack_start(row, sr->enabled, 230, DW_SIZE_AUTO, FALSE, FALSE, 1);
        sr->file = dw_entryfield_new(file ? file : "", 0);
        dw_box_pack_start(row, sr->file, 120, DW_SIZE_AUTO, TRUE, FALSE, 1);
        dwp_button(row, "...", DW_SIGNAL_FUNC(sound_browse), sr);
        dwp_button(row, "Reset", DW_SIGNAL_FUNC(sound_reset), sr);
        dwp_button(row, "Test", DW_SIGNAL_FUNC(sound_test), sr);
        g_free(fp);
        g_free(en);
    }

    /* ---- Status / Idle */
    page = new_page(pd->notebook, "Status / Idle", FALSE);
    pd->idle_reporting = combo(page, "Report idle time:", idle_labels, COUNT(idle_labels),
                               string_index(idle_values, COUNT(idle_values),
                                            purple_prefs_get_string("/purple/away/idle_reporting")));
    pd->away_when_idle = check(page, "Change status to Away when idle",
                               purple_prefs_get_bool("/purple/away/away_when_idle"));
    pd->mins_before_away = spin(page, "Minutes before becoming idle:",
                                purple_prefs_get_int("/purple/away/mins_before_away"), 1, 1440);
    pd->auto_reply = combo(page, "Auto-reply:", reply_labels, COUNT(reply_labels),
                           string_index(reply_values, COUNT(reply_values),
                                        purple_prefs_get_string("/purple/away/auto_reply")));
    mins = g_strdup_printf("System idle source: %s", dwp_idle_backend_name());
    dw_box_pack_start(page, dwp_label(mins), DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 4);
    g_free(mins);

    /* ---- Logging */
    page = new_page(pd->notebook, "Logging", FALSE);
    pd->log_ims = check(page, "Log all instant messages", purple_prefs_get_bool("/purple/logging/log_ims"));
    pd->log_chats = check(page, "Log all chats", purple_prefs_get_bool("/purple/logging/log_chats"));
    pd->log_system = check(page, "Log all status changes to system log",
                           purple_prefs_get_bool("/purple/logging/log_system"));

    /* ---- Network */
    page = new_page(pd->notebook, "Network", FALSE);
    pd->stun = entry(page, "STUN server:", purple_prefs_get_string("/purple/network/stun_server"), FALSE);
    group = dw_groupbox_new(DW_VERT, 2, "Global Proxy");
    dw_box_pack_start(page, group, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 2);
    pd->proxy_type = combo(group, "Type:", proxy_labels, COUNT(proxy_labels),
                           string_index(proxy_values, COUNT(proxy_values), purple_prefs_get_string("/purple/proxy/type")));
    pd->proxy_host = entry(group, "Host:", purple_prefs_get_string("/purple/proxy/host"), FALSE);
    pd->proxy_port = spin(group, "Port:", purple_prefs_get_int("/purple/proxy/port"), 0, 65535);
    pd->proxy_user = entry(group, "User:", purple_prefs_get_string("/purple/proxy/username"), FALSE);
    pd->proxy_pass = entry(group, "Password:", purple_prefs_get_string("/purple/proxy/password"), TRUE);

    buttons = dw_box_new(DW_HORZ, 0);
    dw_box_pack_start(box, buttons, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 2);
    dwp_spacer(buttons);
    dwp_button(buttons, "Save", DW_SIGNAL_FUNC(prefs_save), NULL);
    dwp_button(buttons, "Close", DW_SIGNAL_FUNC(prefs_close), NULL);
    dw_signal_connect(pd->window, DW_SIGNAL_DELETE, DW_SIGNAL_FUNC(prefs_close), NULL);
    dwp_window_present(pd->window, 640, 540);
}
