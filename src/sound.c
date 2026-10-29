/*
 * DWPurple - sounds.
 *
 * Playback methods: "automatic" (Windows: PlaySound; Unix: pw-play, paplay
 * or aplay; OS/2: the configured command, else a beep), "command" (user
 * command, %s = file name), "beep" and "none".  Event filtering follows
 * Pidgin: per-event switches, the "sounds while" status rule, the focus
 * rule and a short mute after sign-on.
 */
#include "dwpurple.h"

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif

static const DwpSoundEvent events[] = {
    { PURPLE_SOUND_BUDDY_ARRIVE,   "login",          "Buddy logs in",                        "login.wav",   TRUE  },
    { PURPLE_SOUND_BUDDY_LEAVE,    "logout",         "Buddy logs out",                       "logout.wav",  TRUE  },
    { PURPLE_SOUND_RECEIVE,        "im_recv",        "Message received",                     "receive.wav", TRUE  },
    { PURPLE_SOUND_FIRST_RECEIVE,  "first_im_recv",  "Message received begins conversation", "alert.wav",   FALSE },
    { PURPLE_SOUND_SEND,           "send_im",        "Message sent",                         "send.wav",    TRUE  },
    { PURPLE_SOUND_CHAT_JOIN,      "join_chat",      "Person enters chat",                   "login.wav",   FALSE },
    { PURPLE_SOUND_CHAT_LEAVE,     "left_chat",      "Person leaves chat",                   "logout.wav",  FALSE },
    { PURPLE_SOUND_CHAT_YOU_SAY,   "send_chat_msg",  "You talk in chat",                     "send.wav",    FALSE },
    { PURPLE_SOUND_CHAT_SAY,       "chat_msg_recv",  "Others talk in chat",                  "receive.wav", FALSE },
    { PURPLE_SOUND_POUNCE_DEFAULT, "pounce_default", "Default buddy pounce sound",           "alert.wav",   TRUE  },
    { PURPLE_SOUND_CHAT_NICK,      "nick_said",      "Someone says your name in chat",       "alert.wav",   FALSE },
    { PURPLE_SOUND_GOT_ATTENTION,  "attention",      "Attention received",                   "alert.wav",   TRUE  },
};
#define N_EVENTS ((int)(sizeof(events) / sizeof(events[0])))

static int sound_handle;
static gint64 login_mute_until = 0;
static gboolean event_reached = FALSE;

const DwpSoundEvent *dwp_sound_events(int *count)
{
    if(count)
        *count = N_EVENTS;
    return events;
}

static const DwpSoundEvent *find_event(PurpleSoundEventID id)
{
    int z;

    for(z = 0; z < N_EVENTS; z++)
        if(events[z].id == id)
            return &events[z];
    return NULL;
}

char *dwp_sound_enabled_pref(const DwpSoundEvent *e)
{
    return g_strdup_printf(DWP_PREF_SOUND_ENABLED "/%s", e->pref);
}

char *dwp_sound_file_pref(const DwpSoundEvent *e)
{
    return g_strdup_printf(DWP_PREF_SOUND_FILE "/%s", e->pref);
}

static gboolean event_enabled(const DwpSoundEvent *e)
{
    char *pref;
    gboolean on;

    if(!e)
        return FALSE;
    pref = dwp_sound_enabled_pref(e);
    on = purple_prefs_get_bool(pref);
    g_free(pref);
    return on;
}

void dwp_sound_add_prefs(void)
{
    int z;

    purple_prefs_add_none(DWP_PREF_SOUND);
    purple_prefs_add_none(DWP_PREF_SOUND_ENABLED);
    purple_prefs_add_none(DWP_PREF_SOUND_FILE);
    purple_prefs_add_string(DWP_PREF_SOUND_METHOD, "automatic");
    purple_prefs_add_string(DWP_PREF_SOUND_COMMAND, "");
    purple_prefs_add_bool(DWP_PREF_SOUND_MUTE, FALSE);
    purple_prefs_add_bool(DWP_PREF_SOUND_FOCUS, TRUE);
    for(z = 0; z < N_EVENTS; z++)
    {
        char *en = dwp_sound_enabled_pref(&events[z]);
        char *file = dwp_sound_file_pref(&events[z]);

        purple_prefs_add_bool(en, events[z].default_enabled);
        purple_prefs_add_path(file, "");
        g_free(en);
        g_free(file);
    }
}

char *dwp_sound_default_file(const DwpSoundEvent *e)
{
    const char *const *dirs = g_get_system_data_dirs();
    char *path;
    int z;

    /* Pidgin installs its sounds as <datadir>/sounds/purple/<file>. */
    for(z = 0; dirs && dirs[z]; z++)
    {
        path = g_build_filename(dirs[z], "sounds", "purple", e->default_file, NULL);
        if(g_file_test(path, G_FILE_TEST_IS_REGULAR))
            return path;
        g_free(path);
    }
    path = g_build_filename(dw_app_dir(), "sounds", e->default_file, NULL);
    if(g_file_test(path, G_FILE_TEST_IS_REGULAR))
        return path;
    g_free(path);
    path = g_build_filename(purple_user_dir(), "sounds", e->default_file, NULL);
    if(g_file_test(path, G_FILE_TEST_IS_REGULAR))
        return path;
    g_free(path);
    return NULL;
}

char *dwp_sound_effective_file(const DwpSoundEvent *e)
{
    char *pref = dwp_sound_file_pref(e);
    const char *custom = purple_prefs_get_path(pref);
    char *ret = (custom && *custom) ? g_strdup(custom) : dwp_sound_default_file(e);

    g_free(pref);
    return ret;
}

gboolean dwp_sound_muted(void)
{
    return purple_prefs_get_bool(DWP_PREF_SOUND_MUTE);
}

void dwp_sound_set_muted(gboolean mute)
{
    purple_prefs_set_bool(DWP_PREF_SOUND_MUTE, mute);
}

/* ---------------------------------------------------------------- players */

static gboolean spawn(char **argv)
{
    GError *error = NULL;

    /* Output is not discarded: player errors stay visible on the terminal. */
    if(!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &error))
    {
        purple_debug_error("sound", "Could not run %s: %s\n", argv[0], error ? error->message : "unknown error");
        if(error)
            g_error_free(error);
        return FALSE;
    }
    return TRUE;
}

static gboolean play_with_command(const char *command, const char *file)
{
    int argc = 0, z;
    char **parsed = NULL;
    GPtrArray *args;
    GError *error = NULL;
    gboolean substituted = FALSE, ok;

    if(!command || !*command)
        return FALSE;
    if(!g_shell_parse_argv(command, &argc, &parsed, &error))
    {
        purple_debug_error("sound", "Bad sound command: %s\n", error ? error->message : "");
        if(error)
            g_error_free(error);
        return FALSE;
    }
    args = g_ptr_array_new_with_free_func(g_free);
    for(z = 0; z < argc; z++)
    {
        if(strstr(parsed[z], "%s"))
        {
            char **parts = g_strsplit(parsed[z], "%s", -1);

            g_ptr_array_add(args, g_strjoinv(file, parts));
            g_strfreev(parts);
            substituted = TRUE;
        }
        else
            g_ptr_array_add(args, g_strdup(parsed[z]));
    }
    g_strfreev(parsed);
    if(!substituted)
        g_ptr_array_add(args, g_strdup(file));
    g_ptr_array_add(args, NULL);
    ok = spawn((char **)args->pdata);
    g_ptr_array_free(args, TRUE);
    return ok;
}

static gboolean play_automatic(const char *file)
{
#ifdef _WIN32
    wchar_t *wfile = g_utf8_to_utf16(file, -1, NULL, NULL, NULL);
    BOOL ok = wfile ? PlaySoundW(wfile, NULL, SND_ASYNC | SND_FILENAME | SND_NODEFAULT) : FALSE;

    g_free(wfile);
    return ok ? TRUE : FALSE;
#elif defined(__OS2__)
    (void)file;
    return FALSE;       /* uses the command from Preferences, else a beep */
#else
    static const char *const players[] = { "pw-play", "paplay", "aplay", NULL };
    int z;

    for(z = 0; players[z]; z++)
    {
        char *path, *argv[3];

        if(strcmp(players[z], "aplay") == 0 && !g_str_has_suffix(file, ".wav") && !g_str_has_suffix(file, ".WAV"))
            continue;
        path = g_find_program_in_path(players[z]);
        if(!path)
            continue;
        g_free(path);
        argv[0] = (char *)players[z];
        argv[1] = (char *)file;
        argv[2] = NULL;
        return spawn(argv);
    }
    return FALSE;
#endif
}

static void play_file(const char *file)
{
    const char *method = purple_prefs_get_string(DWP_PREF_SOUND_METHOD);

    if(purple_strequal(method, "none"))
        return;
    if(purple_strequal(method, "beep") || !file || !*file || !g_file_test(file, G_FILE_TEST_IS_REGULAR))
    {
        if(!purple_strequal(method, "beep"))
            purple_debug_warning("sound", "Sound file not found: %s\n", file ? file : "(none)");
        dw_beep(880, 120);
        return;
    }
    if(purple_strequal(method, "command"))
    {
        if(!play_with_command(purple_prefs_get_string(DWP_PREF_SOUND_COMMAND), file))
            dw_beep(880, 120);
        return;
    }
    if(!play_automatic(file) &&
       !play_with_command(purple_prefs_get_string(DWP_PREF_SOUND_COMMAND), file))
        dw_beep(880, 120);
}

void dwp_sound_test(const char *file)
{
    play_file(file);
}

static void ui_play_file(const char *filename)
{
    if(!dwp_sound_muted())
        play_file(filename);
}

static void ui_play_event(PurpleSoundEventID id)
{
    const DwpSoundEvent *e = find_event(id);
    char *file;

    event_reached = TRUE;
    if(dwp_sound_muted() || !event_enabled(e))
        return;
    file = dwp_sound_effective_file(e);
    purple_debug_info("sound", "Playing '%s': %s (method %s)\n", e->pref, file ? file : "(no file)",
                      purple_prefs_get_string(DWP_PREF_SOUND_METHOD));
    play_file(file);
    g_free(file);
}

/* ------------------------------------------------------------ event rules */

static gboolean status_allows(PurpleAccount *account)
{
    int rule = purple_prefs_get_int("/purple/sound/while_status");
    PurpleStatus *status;
    gboolean available;

    if(rule == 3 || !account)
        return TRUE;
    status = purple_account_get_active_status(account);
    if(!status || !purple_status_is_online(status))
        return TRUE;
    available = purple_status_is_available(status);
    return (available && rule == 1) || (!available && rule == 2);
}

static void request_event(PurpleSoundEventID id, PurpleAccount *account, PurpleConversation *conv)
{
    const DwpSoundEvent *e = find_event(id);
    const char *name = e ? e->pref : "unknown";

    if(dwp_sound_muted())
    {
        purple_debug_info("sound", "'%s' not played: sounds are muted\n", name);
        return;
    }
    if(!event_enabled(e))
    {
        purple_debug_info("sound", "'%s' not played: switched off in Preferences\n", name);
        return;
    }
    if(conv && !purple_prefs_get_bool(DWP_PREF_SOUND_FOCUS) && purple_conversation_has_focus(conv))
    {
        purple_debug_info("sound", "'%s' not played: the conversation has focus\n", name);
        return;
    }
    if(!status_allows(account))
    {
        purple_debug_info("sound", "'%s' not played: account status rule\n", name);
        return;
    }
    event_reached = FALSE;
    purple_sound_play_event(id, account);
    if(!event_reached)
        purple_debug_info("sound", "'%s' not played: suppressed by libpurple "
                          "(same sound within 2 seconds, or a plugin)\n", name);
}

static gboolean login_muted(void)
{
    return g_get_monotonic_time() < login_mute_until;
}

static void play_conv_event(PurpleConversation *conv, PurpleSoundEventID id)
{
    request_event(id, conv ? purple_conversation_get_account(conv) : NULL, conv);
}

static void on_account_signed_on(PurpleAccount *account, gpointer data)
{
    (void)account;
    (void)data;
    login_mute_until = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
}

static void on_buddy_signed_on(PurpleBuddy *buddy, gpointer data)
{
    (void)data;
    if(!login_muted())
        request_event(PURPLE_SOUND_BUDDY_ARRIVE, purple_buddy_get_account(buddy), NULL);
}

static void on_buddy_signed_off(PurpleBuddy *buddy, gpointer data)
{
    (void)data;
    if(!login_muted())
        request_event(PURPLE_SOUND_BUDDY_LEAVE, purple_buddy_get_account(buddy), NULL);
}

static void on_received_im(PurpleAccount *account, char *sender, char *message, PurpleConversation *conv,
                           PurpleMessageFlags flags, gpointer data)
{
    (void)sender;
    (void)message;
    (void)data;
    if(flags & PURPLE_MESSAGE_DELAYED)
        return;
    if(!conv)
    {
        request_event(event_enabled(find_event(PURPLE_SOUND_FIRST_RECEIVE))
                      ? PURPLE_SOUND_FIRST_RECEIVE : PURPLE_SOUND_RECEIVE, account, NULL);
        return;
    }
    play_conv_event(conv, PURPLE_SOUND_RECEIVE);
}

static void on_sent_im(PurpleAccount *account, const char *receiver, const char *message, gpointer data)
{
    (void)message;
    (void)data;
    play_conv_event(purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM, receiver, account),
                    PURPLE_SOUND_SEND);
}

static void on_received_chat(PurpleAccount *account, char *sender, char *message, PurpleConversation *conv,
                             PurpleMessageFlags flags, gpointer data)
{
    PurpleConvChat *chat;
    const char *nick;

    (void)account;
    (void)message;
    (void)data;
    if((flags & PURPLE_MESSAGE_DELAYED) || !conv)
        return;
    chat = PURPLE_CONV_CHAT(conv);
    nick = purple_conv_chat_get_nick(chat);
    if(sender && nick && purple_utf8_strcasecmp(sender, nick) == 0)
        return;
    if(sender && purple_conv_chat_is_user_ignored(chat, sender))
        return;
    if((flags & PURPLE_MESSAGE_NICK) && event_enabled(find_event(PURPLE_SOUND_CHAT_NICK)))
        play_conv_event(conv, PURPLE_SOUND_CHAT_NICK);
    else
        play_conv_event(conv, PURPLE_SOUND_CHAT_SAY);
}

static void on_sent_chat(PurpleAccount *account, const char *message, int id, gpointer data)
{
    PurpleConnection *gc = purple_account_get_connection(account);

    (void)message;
    (void)data;
    play_conv_event(gc ? purple_find_chat(gc, id) : NULL, PURPLE_SOUND_CHAT_YOU_SAY);
}

static void on_chat_buddy_joined(PurpleConversation *conv, const char *name, PurpleConvChatBuddyFlags flags,
                                 gboolean new_arrival, gpointer data)
{
    (void)name;
    (void)flags;
    (void)data;
    if(new_arrival)
        play_conv_event(conv, PURPLE_SOUND_CHAT_JOIN);
}

static void on_chat_buddy_left(PurpleConversation *conv, const char *name, const char *reason, gpointer data)
{
    (void)name;
    (void)reason;
    (void)data;
    play_conv_event(conv, PURPLE_SOUND_CHAT_LEAVE);
}

static void on_got_attention(PurpleAccount *account, const char *who, PurpleConversation *conv,
                             guint type, gpointer data)
{
    (void)who;
    (void)type;
    (void)data;
    if(conv)
        play_conv_event(conv, PURPLE_SOUND_GOT_ATTENTION);
    else
        request_event(PURPLE_SOUND_GOT_ATTENTION, account, NULL);
}

static void ui_init(void)
{
    void *accounts = purple_accounts_get_handle();
    void *blist = purple_blist_get_handle();
    void *convs = purple_conversations_get_handle();

    purple_signal_connect(accounts, "account-signed-on", &sound_handle, PURPLE_CALLBACK(on_account_signed_on), NULL);
    purple_signal_connect(blist, "buddy-signed-on", &sound_handle, PURPLE_CALLBACK(on_buddy_signed_on), NULL);
    purple_signal_connect(blist, "buddy-signed-off", &sound_handle, PURPLE_CALLBACK(on_buddy_signed_off), NULL);
    purple_signal_connect(convs, "received-im-msg", &sound_handle, PURPLE_CALLBACK(on_received_im), NULL);
    purple_signal_connect(convs, "sent-im-msg", &sound_handle, PURPLE_CALLBACK(on_sent_im), NULL);
    purple_signal_connect(convs, "received-chat-msg", &sound_handle, PURPLE_CALLBACK(on_received_chat), NULL);
    purple_signal_connect(convs, "sent-chat-msg", &sound_handle, PURPLE_CALLBACK(on_sent_chat), NULL);
    purple_signal_connect(convs, "chat-buddy-joined", &sound_handle, PURPLE_CALLBACK(on_chat_buddy_joined), NULL);
    purple_signal_connect(convs, "chat-buddy-left", &sound_handle, PURPLE_CALLBACK(on_chat_buddy_left), NULL);
    purple_signal_connect(convs, "got-attention", &sound_handle, PURPLE_CALLBACK(on_got_attention), NULL);
}

static void ui_uninit(void)
{
    purple_signals_disconnect_by_handle(&sound_handle);
}

PurpleSoundUiOps *dwp_sound_ui_ops(void)
{
    static PurpleSoundUiOps ops;
    static gboolean ready = FALSE;

    if(!ready)
    {
        memset(&ops, 0, sizeof(ops));
        ops.init = ui_init;
        ops.uninit = ui_uninit;
        ops.play_file = ui_play_file;
        ops.play_event = ui_play_event;
        ready = TRUE;
    }
    return &ops;
}
