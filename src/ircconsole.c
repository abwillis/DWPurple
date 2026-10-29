/*
 * DWPurple - IRC server console.
 *
 * libpurple's IRC protocol has no server window: lines that are not aimed
 * at an open channel or query (MOTD, notices, numeric replies, errors)
 * only go to the debug log, and Pidgin behaves the same way.  This window
 * shows them by listening to the IRC plugin's "irc-receiving-text" signal
 * and sends typed commands with the protocol's send_raw.  It opens when an
 * IRC account starts signing on (so the welcome and MOTD are captured) and
 * can be reopened from the account's menu.
 *
 * Input:
 *   /join #chan, /nick x, /whois x, /list, /motd ...  sent as JOIN #chan ...
 *   /msg nick text, /query nick                       open an IM window
 *   /quote LINE, /raw LINE                            LINE sent unchanged
 *   anything without a leading /                      sent unchanged
 */
#include "dwpurple.h"

#include <ctype.h>

#define IRC_PRPL_ID        "prpl-irc"
#define CONSOLE_MAX_CHARS  100000

typedef struct {
    PurpleAccount *account;
    HWND window, history, entry;
    unsigned int pos;
    unsigned long chars;
} Console;

static GHashTable *consoles = NULL;      /* account -> Console */
static int console_handle;
static gboolean irc_hooked = FALSE;

static gboolean is_irc(PurpleAccount *account)
{
    return account && purple_strequal(purple_account_get_protocol_id(account), IRC_PRPL_ID);
}

static char *server_name(PurpleAccount *account)
{
    const char *user = purple_account_get_username(account);
    const char *at = user ? strrchr(user, '@') : NULL;

    return g_strdup(at && at[1] ? at + 1 : (user ? user : "IRC"));
}

/* ---------------------------------------------------------------- output */

static void console_write(Console *c, const char *line)
{
    char ts[16];
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char *text;

    if(!c->history)
        return;
    if(!tm || !strftime(ts, sizeof(ts), "%H:%M:%S", tm))
        strcpy(ts, "--:--:--");
    if(c->chars > CONSOLE_MAX_CHARS)
    {
        dw_mle_clear(c->history);
        c->pos = 0;
        c->chars = 0;
    }
    text = g_strdup_printf("(%s) %s\r\n", ts, line);
    if(!g_utf8_validate(text, -1, NULL))
    {
        char *fixed = purple_utf8_salvage(text);

        g_free(text);
        text = fixed;
    }
    c->pos = dw_mle_import(c->history, text, c->pos);
    c->chars += strlen(text);
    dw_mle_set_cursor(c->history, c->pos);
    g_free(text);
}

static gboolean is_channel(const char *s)
{
    return s && (*s == '#' || *s == '&' || *s == '+' || *s == '!');
}

/* Turn a raw server line into console text; NULL for lines that already
 * show up elsewhere (channel and query traffic, keep-alives, replies that
 * libpurple consumes for the buddy list and room list). */
static char *format_line(const char *raw)
{
    static const char *const hidden[] = {
        "PING", "PONG", "JOIN", "PART", "QUIT", "NICK", "KICK", "TOPIC", "CAP",
        "AUTHENTICATE", "303", "315", "321", "322", "323", "332", "333", "352",
        "353", "366", NULL
    };
    char *line = g_strdup(raw), *p = line, *prefix = NULL, *cmd, *trailing = NULL, *sp;
    GPtrArray *params = g_ptr_array_new();
    GString *out;
    char *ret = NULL;
    guint z;

    g_strchomp(line);
    if(*p == ':')
    {
        prefix = ++p;
        if(!(sp = strchr(p, ' ')))
            goto done;
        *sp = '\0';
        p = sp + 1;
    }
    while(*p == ' ')
        p++;
    cmd = p;
    if(!*cmd)
        goto done;
    if((sp = strchr(p, ' ')))
    {
        *sp = '\0';
        p = sp + 1;
        while(*p)
        {
            while(*p == ' ')
                p++;
            if(*p == ':')
            {
                trailing = p + 1;
                break;
            }
            if(!*p)
                break;
            g_ptr_array_add(params, p);
            if(!(sp = strchr(p, ' ')))
                break;
            *sp = '\0';
            p = sp + 1;
        }
    }

    for(z = 0; hidden[z]; z++)
        if(g_ascii_strcasecmp(cmd, hidden[z]) == 0)
            goto done;
    /* MODE and PRIVMSG/NOTICE to a channel belong to the channel window;
     * PRIVMSG to us opens a query window. */
    if((g_ascii_strcasecmp(cmd, "MODE") == 0 || g_ascii_strcasecmp(cmd, "PRIVMSG") == 0 ||
        g_ascii_strcasecmp(cmd, "NOTICE") == 0) &&
       params->len && is_channel(g_ptr_array_index(params, 0)))
        goto done;
    if(g_ascii_strcasecmp(cmd, "PRIVMSG") == 0)
        goto done;

    out = g_string_new(NULL);
    if(g_ascii_strcasecmp(cmd, "NOTICE") == 0)
    {
        char *bang = prefix ? strchr(prefix, '!') : NULL;

        if(bang)
            *bang = '\0';
        g_string_append_printf(out, "-%s- %s", prefix ? prefix : "server", trailing ? trailing : "");
    }
    else if(strlen(cmd) == 3 && isdigit((unsigned char)cmd[0]) &&
            isdigit((unsigned char)cmd[1]) && isdigit((unsigned char)cmd[2]))
    {
        /* Numeric: drop the first parameter (our own nick). */
        for(z = 1; z < params->len; z++)
            g_string_append_printf(out, "%s%s", out->len ? " " : "", (char *)g_ptr_array_index(params, z));
        if(trailing)
            g_string_append_printf(out, "%s%s", out->len ? " " : "", trailing);
        if(!out->len)
            g_string_append(out, cmd);
    }
    else
    {
        g_string_append(out, cmd);
        for(z = 0; z < params->len; z++)
            g_string_append_printf(out, " %s", (char *)g_ptr_array_index(params, z));
        if(trailing)
            g_string_append_printf(out, " %s", trailing);
    }
    ret = g_string_free(out, FALSE);

done:
    g_ptr_array_free(params, TRUE);
    g_free(line);
    return ret;
}

/* ---------------------------------------------------------------- window */

static void console_free(gpointer p)
{
    g_free(p);
}

static int DWSIGNAL console_close(HWND w, void *data)
{
    Console *c = data;

    (void)w;
    if(consoles && g_hash_table_lookup(consoles, c->account) == c)
        g_hash_table_steal(consoles, c->account);
    c->history = c->entry = 0;
    dwp_window_close_later(c->window, console_free, c);
    return TRUE;
}

static gboolean send_line(Console *c, const char *line)
{
    PurpleConnection *gc = purple_account_get_connection(c->account);
    PurplePluginProtocolInfo *info = dwp_prpl_info(gc);
    char *wire;

    if(!gc || purple_connection_get_state(gc) != PURPLE_CONNECTED)
    {
        console_write(c, "!! Not connected.");
        return FALSE;
    }
    if(!info || !info->send_raw)
    {
        console_write(c, "!! This protocol cannot send raw commands.");
        return FALSE;
    }
    wire = g_strdup_printf("%s\r\n", line);
    info->send_raw(gc, wire, -1);
    g_free(wire);
    return TRUE;
}

static void handle_input(Console *c, const char *typed)
{
    char *text = g_strstrip(g_strdup(typed)), *p, *verb, *rest, *line = NULL, *echo;

    /* One command per entry: never let typed newlines start a second one. */
    for(p = text; *p; p++)
        if(*p == '\r' || *p == '\n')
            *p = ' ';
    if(!*text)
    {
        g_free(text);
        return;
    }
    if(text[0] != '/')
        line = g_strdup(text);
    else
    {
        verb = text + 1;
        rest = strchr(verb, ' ');
        if(rest)
        {
            *rest++ = '\0';
            while(*rest == ' ')
                rest++;
        }
        else
            rest = verb + strlen(verb);

        if(!g_ascii_strcasecmp(verb, "quote") || !g_ascii_strcasecmp(verb, "raw"))
            line = g_strdup(rest);
        else if(!g_ascii_strcasecmp(verb, "msg") || !g_ascii_strcasecmp(verb, "query"))
        {
            char *msg = strchr(rest, ' ');

            if(msg)
            {
                *msg++ = '\0';
                while(*msg == ' ')
                    msg++;
            }
            if(!*rest)
                console_write(c, "!! Usage: /msg <nick> [message]");
            else if(!purple_account_is_connected(c->account))
                console_write(c, "!! Not connected.");
            else
            {
                PurpleConversation *conv;

                dwp_open_im(c->account, rest);
                conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM, rest, c->account);
                if(conv && msg && *msg)
                {
                    char *html = g_markup_escape_text(msg, -1);

                    purple_conv_im_send(PURPLE_CONV_IM(conv), html);
                    g_free(html);
                }
            }
            g_free(text);
            return;
        }
        else if(*verb)
        {
            char *up = g_ascii_strup(verb, -1);

            line = *rest ? g_strdup_printf("%s %s", up, rest) : g_strdup(up);
            g_free(up);
        }
    }
    if(line && *line && send_line(c, line))
    {
        echo = g_strdup_printf("> %s", line);
        console_write(c, echo);
        g_free(echo);
    }
    g_free(line);
    g_free(text);
}

static int DWSIGNAL console_send(HWND w, void *data)
{
    Console *c = data;
    char *text;

    (void)w;
    if(!c->entry)
        return TRUE;
    text = dwp_get_text(c->entry);
    dw_window_set_text(c->entry, "");
    handle_input(c, text);
    g_free(text);
    dw_window_set_focus(c->entry);
    return TRUE;
}

static Console *console_open(PurpleAccount *account, gboolean raise)
{
    Console *c;
    HWND box, row, sendb;
    char *server, *title;

    if(!consoles)
        consoles = g_hash_table_new(g_direct_hash, g_direct_equal);
    if((c = g_hash_table_lookup(consoles, account)))
    {
        dw_window_show(c->window);
        if(raise)
            dw_window_raise(c->window);
        return c;
    }
    c = g_new0(Console, 1);
    c->account = account;
    server = server_name(account);
    title = g_strdup_printf("%s - %s", server, purple_account_get_username(account));
    c->window = dwp_window_new(title, 0, 0);
    g_free(title);
    g_free(server);
    box = dw_box_new(DW_VERT, 2);
    dw_box_pack_start(c->window, box, 0, 0, TRUE, TRUE, 0);
    c->history = dw_mle_new(0);
    dw_mle_set_editable(c->history, FALSE);
    dw_mle_set_word_wrap(c->history, TRUE);
    dw_box_pack_start(box, c->history, 560, 320, TRUE, TRUE, 2);
    row = dw_box_new(DW_HORZ, 0);
    dw_box_pack_start(box, row, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 2);
    c->entry = dw_entryfield_new("", 0);
    dw_window_set_tooltip(c->entry, "/join #channel, /msg nick text, /whois nick, /list, /quote RAW");
    dw_box_pack_start(row, c->entry, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 1);
    sendb = dwp_button(row, "Send", DW_SIGNAL_FUNC(console_send), c);
    dw_window_click_default(c->entry, sendb);
    dw_signal_connect(c->window, DW_SIGNAL_DELETE, DW_SIGNAL_FUNC(console_close), c);
    g_hash_table_insert(consoles, account, c);
    dwp_window_present(c->window, 640, 440);
    if(raise)
        dw_window_set_focus(c->entry);
    return c;
}

void dwp_irc_console_show(PurpleAccount *account)
{
    if(dwp_account_exists(account) && is_irc(account))
        console_open(account, TRUE);
}

gboolean dwp_irc_console_supported(PurpleAccount *account)
{
    return is_irc(account);
}

/* --------------------------------------------------------------- signals */

static void on_receiving_text(PurpleConnection *gc, char **text, gpointer data)
{
    PurpleAccount *account;
    Console *c;
    char *shown;

    (void)data;
    if(!consoles || !gc || !text || !*text)
        return;
    account = purple_connection_get_account(gc);
    if(!(c = g_hash_table_lookup(consoles, account)))
        return;
    if((shown = format_line(*text)))
    {
        console_write(c, shown);
        g_free(shown);
    }
}

static void hook_irc(void)
{
    PurplePlugin *irc;

    if(irc_hooked || !(irc = purple_plugins_find_with_id(IRC_PRPL_ID)))
        return;
    purple_signal_connect(irc, "irc-receiving-text", &console_handle,
                          PURPLE_CALLBACK(on_receiving_text), NULL);
    irc_hooked = TRUE;
}

static void on_signing_on(PurpleConnection *gc, gpointer data)
{
    PurpleAccount *account = purple_connection_get_account(gc);
    Console *c;
    char *server, *msg;

    (void)data;
    if(!is_irc(account))
        return;
    hook_irc();
    c = console_open(account, FALSE);
    server = server_name(account);
    msg = g_strdup_printf("Connecting to %s...", server);
    console_write(c, msg);
    g_free(msg);
    g_free(server);
}

static void on_signed_on(PurpleConnection *gc, gpointer data)
{
    PurpleAccount *account = purple_connection_get_account(gc);
    Console *c;
    char *server, *msg;

    (void)data;
    if(!is_irc(account))
        return;
    hook_irc();
    c = console_open(account, FALSE);
    server = server_name(account);
    msg = g_strdup_printf("Connected to %s as %s.", server, purple_connection_get_display_name(gc));
    console_write(c, msg);
    g_free(msg);
    g_free(server);
}

static void on_signed_off(PurpleConnection *gc, gpointer data)
{
    Console *c;

    (void)data;
    if(consoles && (c = g_hash_table_lookup(consoles, purple_connection_get_account(gc))))
        console_write(c, "Disconnected.");
}

static void on_account_removed(PurpleAccount *account, gpointer data)
{
    Console *c;

    (void)data;
    if(consoles && (c = g_hash_table_lookup(consoles, account)))
        console_close(0, c);
}

void dwp_irc_console_init(void)
{
    void *conns = purple_connections_get_handle();

    purple_signal_connect(conns, "signing-on", &console_handle, PURPLE_CALLBACK(on_signing_on), NULL);
    purple_signal_connect(conns, "signed-on", &console_handle, PURPLE_CALLBACK(on_signed_on), NULL);
    purple_signal_connect(conns, "signed-off", &console_handle, PURPLE_CALLBACK(on_signed_off), NULL);
    purple_signal_connect(purple_accounts_get_handle(), "account-removed", &console_handle,
                          PURPLE_CALLBACK(on_account_removed), NULL);
    hook_irc();
}
