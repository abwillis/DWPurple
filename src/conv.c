/*
 * DWPurple - tabbed conversation window and conversation UI ops.
 *
 * History is a read-only MLE: Dynamic Windows has no rich text widget on
 * every platform (no HTML widget on OS/2), so messages are converted from
 * HTML to text.  The MLE insertion point is tracked explicitly because
 * dw_mle_import(-1) inserts at the start on OS/2.
 */
#include "dwpurple.h"

#define MAX_LINES       2000
#define RERENDER_LINES  300
#define MAX_MLE_CHARS   60000
#define INPUT_HISTORY   50

enum { ACT_NONE = 0, ACT_TYPING, ACT_EVENT, ACT_MESSAGE, ACT_NICK };

static const char *const reactions_key = "dwpurple-show-reactions";
static const char *const emoji_key = "dwpurple-emoji-codes";

typedef struct {
    PurpleMessageFlags flags;
    time_t mtime;
    char *name;
    char *text;
    gboolean me, reaction;
} Line;

typedef struct {
    PurpleConversation *conv;
    unsigned long page;
    HWND box, title, subtitle, history, users, user_count, typing, input;
    unsigned int pos;
    unsigned long chars;
    GQueue lines;
    int activity;
    gboolean show_reactions, emoji_codes;
    int hidden_reactions;
    GPtrArray *sent;
    int sent_pos;
    gboolean users_pending, typing_pending;
} ConvView;

typedef struct {
    HWND window, notebook;
    HMENUI menubar, options_menu;
    GList *views;
    gboolean visible;
    unsigned long current;
} ConvWindow;

static ConvWindow cw;
static int conv_handle;
static PurpleCmdId cmd_ids[4];

static void view_update_tab(ConvView *v);
static void view_render_line(ConvView *v, Line *line, gboolean live);
static void window_update_title(void);

/* ================================================================ helpers */

static gboolean view_is_chat(ConvView *v)
{
    return purple_conversation_get_type(v->conv) == PURPLE_CONV_TYPE_CHAT;
}

static ConvView *view_from_conv(PurpleConversation *conv)
{
    return conv ? (ConvView *)conv->ui_data : NULL;
}

static gboolean view_alive(ConvView *v)
{
    return v && g_list_find(cw.views, v) && dwp_conversation_exists(v->conv) && v->conv->ui_data == v;
}

static ConvView *current_view(void)
{
    GList *l;
    unsigned long page;

    if(!cw.notebook)
        return NULL;
    page = dw_notebook_page_get(cw.notebook);
    for(l = cw.views; l; l = l->next)
        if(((ConvView *)l->data)->page == page)
            return l->data;
    return cw.views ? cw.views->data : NULL;
}

static gboolean view_visible(ConvView *v)
{
    return cw.visible && v && current_view() == v;
}

static void line_free(gpointer p)
{
    Line *line = p;

    g_free(line->name);
    g_free(line->text);
    g_free(line);
}

static void clear_lines(ConvView *v)
{
    while(!g_queue_is_empty(&v->lines))
        line_free(g_queue_pop_head(&v->lines));
}

static PurpleBlistNode *settings_node(ConvView *v)
{
    PurpleAccount *account = purple_conversation_get_account(v->conv);
    const char *name = purple_conversation_get_name(v->conv);

    if(view_is_chat(v))
    {
        PurpleChat *chat = purple_blist_find_chat(account, name);

        return chat ? (PurpleBlistNode *)chat : NULL;
    }
    else
    {
        PurpleBuddy *buddy = purple_find_buddy(account, name);

        return buddy ? (PurpleBlistNode *)buddy : NULL;
    }
}

static gboolean node_option(PurpleBlistNode *node, const char *key, gboolean def)
{
    const char *v = node ? purple_blist_node_get_string(node, key) : NULL;

    if(!v)
        return def;
    return strcmp(v, "off") != 0;
}

static void load_display_options(ConvView *v)
{
    PurpleBlistNode *node = settings_node(v);

    v->show_reactions = node_option(node, reactions_key, purple_prefs_get_bool(DWP_PREF_SHOW_REACTIONS));
    v->emoji_codes = node_option(node, emoji_key, purple_prefs_get_bool(DWP_PREF_EMOJI_CODES));
}

static void save_display_option(ConvView *v, const char *key, gboolean on)
{
    PurpleBlistNode *node = settings_node(v);

    if(node)
        purple_blist_node_set_string(node, key, on ? "on" : "off");
}

static void system_message(ConvView *v, const char *text, gboolean error)
{
    char *html = g_markup_escape_text(text, -1);

    purple_conversation_write(v->conv, "", html,
                              (error ? PURPLE_MESSAGE_ERROR : PURPLE_MESSAGE_SYSTEM) | PURPLE_MESSAGE_NO_LOG,
                              time(NULL));
    g_free(html);
}

/* ================================================================ history */

static void history_append(ConvView *v, const char *text)
{
    v->pos = dw_mle_import(v->history, text, v->pos);
    v->chars += strlen(text);
}

static void view_rerender(ConvView *v)
{
    GList *l;
    int skip;

    dw_mle_clear(v->history);
    v->pos = 0;
    v->chars = 0;
    v->hidden_reactions = 0;
    skip = (int)g_queue_get_length(&v->lines) - RERENDER_LINES;
    for(l = v->lines.head; l; l = l->next)
    {
        if(skip-- > 0)
            continue;
        view_render_line(v, l->data, FALSE);
    }
    dw_mle_set_cursor(v->history, v->pos);
}

static void view_set_activity(ConvView *v, int activity)
{
    if(v->activity == activity)
        return;
    v->activity = activity;
    view_update_tab(v);
    window_update_title();
    dwp_tray_update();
}

typedef struct {
    PurpleConversation *conv;
} NotifyTarget;

static GPtrArray *notify_targets = NULL;

static int DWSIGNAL notification_clicked(HWND w, void *data)
{
    NotifyTarget *t = data;

    (void)w;
    if(dwp_conversation_exists(t->conv))
        purple_conversation_present(t->conv);
    return TRUE;
}

static void send_notification(ConvView *v, Line *line)
{
    HWND note;
    NotifyTarget *t;
    char *title;

    if(!purple_prefs_get_bool(DWP_PREF_NOTIFY_MSGS) ||
       dw_feature_get(DW_FEATURE_NOTIFICATION) != DW_FEATURE_ENABLED)
        return;
    title = g_strdup_printf("%s (%s)", line->name ? line->name : "",
                            purple_conversation_get_title(v->conv));
    note = dw_notification_new(title, NULL, "%s", line->text);
    g_free(title);
    if(!note)
        return;
    if(!notify_targets)
        notify_targets = g_ptr_array_new_with_free_func(g_free);
    /* Keep a bounded number of click targets alive. */
    if(notify_targets->len > 32)
        g_ptr_array_remove_index(notify_targets, 0);
    t = g_new0(NotifyTarget, 1);
    t->conv = v->conv;
    g_ptr_array_add(notify_targets, t);
    dw_signal_connect(note, DW_SIGNAL_CLICKED, DW_SIGNAL_FUNC(notification_clicked), t);
    dw_notification_send(note);
}

static void view_render_line(ConvView *v, Line *line, gboolean live)
{
    GString *out;
    char *text;

    if(line->reaction && !v->show_reactions)
    {
        v->hidden_reactions++;
        return;
    }
    out = g_string_new(NULL);
    if(purple_prefs_get_bool(DWP_PREF_TIMESTAMPS))
    {
        struct tm *tm = localtime(&line->mtime);
        time_t now = time(NULL);
        struct tm *tdp;
        struct tm msg, today;
        char buf[64];

        /* localtime() returns one static buffer: copy before the second call. */
        if(tm && (msg = *tm, (tdp = localtime(&now)) != NULL))
        {
            gboolean date;
            today = *tdp;
            date = (line->flags & PURPLE_MESSAGE_DELAYED) ||
                   msg.tm_yday != today.tm_yday || msg.tm_year != today.tm_year;
            strftime(buf, sizeof(buf), date ? "(%Y-%m-%d %H:%M:%S) " : "(%H:%M:%S) ", &msg);
            g_string_append(out, buf);
        }
    }
    text = dwp_html_to_text(line->text, v->emoji_codes);
    if((line->flags & (PURPLE_MESSAGE_SYSTEM | PURPLE_MESSAGE_ERROR | PURPLE_MESSAGE_NOTIFY)) ||
       !line->name || !*line->name)
    {
        if(line->flags & PURPLE_MESSAGE_ERROR)
            g_string_append(out, "!! ");
        g_string_append(out, text);
    }
    else
    {
        const char *auto_resp = (line->flags & PURPLE_MESSAGE_AUTO_RESP) ? " <AUTO-REPLY>" : "";

        if(line->flags & PURPLE_MESSAGE_NICK)
            g_string_append(out, ">> ");
        if(line->me)
            g_string_append_printf(out, "***%s%s %s", line->name, auto_resp, text);
        else if(line->reaction)
            g_string_append_printf(out, "%s%s: [%s]", line->name, auto_resp, text);
        else
            g_string_append_printf(out, "%s%s: %s", line->name, auto_resp, text);
    }
    g_free(text);
    /* Normalise line endings for the platform edit controls. */
    {
        char **parts = g_strsplit(out->str, "\n", -1);
        char *joined = g_strjoinv("\r\n", parts);

        g_strfreev(parts);
        g_string_assign(out, joined);
        g_free(joined);
    }
    g_string_append(out, "\r\n");
    history_append(v, out->str);
    g_string_free(out, TRUE);

    if(!live)
        return;
    if(v->chars > MAX_MLE_CHARS)
        view_rerender(v);
    else
        dw_mle_set_cursor(v->history, v->pos);

    if(!view_visible(v))
    {
        int a = ACT_NONE;

        if(line->reaction)
            a = ACT_EVENT;
        else if(line->flags & PURPLE_MESSAGE_RECV)
            a = ((line->flags & PURPLE_MESSAGE_NICK) || !view_is_chat(v)) ? ACT_NICK : ACT_MESSAGE;
        else if(line->flags & (PURPLE_MESSAGE_SYSTEM | PURPLE_MESSAGE_ERROR))
            a = ACT_EVENT;
        if(a > v->activity)
            view_set_activity(v, a);
        if((line->flags & PURPLE_MESSAGE_RECV) && !(line->flags & PURPLE_MESSAGE_DELAYED) && a == ACT_NICK)
        {
            char *plain = dwp_html_to_text(line->text, v->emoji_codes);
            Line copy = *line;

            copy.text = plain;
            send_notification(v, &copy);
            g_free(plain);
        }
    }
}

static void view_write(ConvView *v, const char *who, const char *alias, const char *message,
                       PurpleMessageFlags flags, time_t mtime)
{
    Line *line = g_new0(Line, 1);
    char *copy, *plain;

    line->flags = flags;
    line->mtime = mtime ? mtime : time(NULL);
    line->name = g_strdup(alias && *alias ? alias : (who ? who : ""));
    copy = g_strdup(message ? message : "");
    line->me = purple_message_meify(copy, -1);
    line->text = copy;
    if(!(flags & (PURPLE_MESSAGE_SYSTEM | PURPLE_MESSAGE_ERROR)))
    {
        plain = purple_markup_strip_html(copy);
        line->reaction = dwp_is_reaction(plain);
        g_free(plain);
    }
    g_queue_push_tail(&v->lines, line);
    while(g_queue_get_length(&v->lines) > MAX_LINES)
        line_free(g_queue_pop_head(&v->lines));
    view_render_line(v, line, TRUE);
}

/* ============================================================ header/tab */

static char *view_title(ConvView *v)
{
    return g_strdup(purple_conversation_get_title(v->conv));
}

static void view_update_tab(ConvView *v)
{
    static const char *const prefix[] = { "", "(typing) ", "+ ", "* ", "! " };
    char *title = view_title(v);
    char *text = g_strdup_printf("%s%s", prefix[v->activity], title);

    dw_notebook_page_set_text(cw.notebook, v->page, text);
    g_free(text);
    g_free(title);
}

static void view_update_header(ConvView *v)
{
    PurpleAccount *account = purple_conversation_get_account(v->conv);
    char *title = view_title(v);
    char *acct = dwp_account_display_name(account);
    GString *sub = g_string_new(NULL);

    dw_window_set_text(v->title, title);
    if(view_is_chat(v))
    {
        PurpleConvChat *chat = PURPLE_CONV_CHAT(v->conv);
        const char *topic = purple_conv_chat_get_topic(chat);

        if(purple_conv_chat_has_left(chat))
            g_string_append(sub, "You are not in this chat.");
        else if(topic && *topic)
        {
            char *plain = purple_markup_strip_html(topic);
            char *p;

            for(p = plain; *p; p++)
                if(*p == '\n' || *p == '\r')
                    *p = ' ';
            g_string_append_printf(sub, "Topic: %s", plain);
            g_free(plain);
        }
        else
            g_string_append(sub, acct);
    }
    else
    {
        const char *name = purple_conversation_get_name(v->conv);
        PurpleBuddy *buddy = purple_find_buddy(account, name);

        if(buddy && PURPLE_BUDDY_IS_ONLINE(buddy))
        {
            PurpleStatus *status = purple_presence_get_active_status(purple_buddy_get_presence(buddy));

            if(status)
            {
                const char *msg = purple_status_get_attr_string(status, "message");

                g_string_append(sub, purple_status_get_name(status));
                if(msg && *msg)
                {
                    char *plain = purple_markup_strip_html(msg);

                    g_string_append_printf(sub, ": %s", plain);
                    g_free(plain);
                }
            }
        }
        else if(buddy)
            g_string_append(sub, "Offline");
        g_string_append_printf(sub, "%svia %s", sub->len ? "  -  " : "", acct);
    }
    dw_window_set_text(v->subtitle, sub->str);
    g_string_free(sub, TRUE);
    g_free(acct);
    g_free(title);
    view_update_tab(v);
    if(current_view() == v)
        window_update_title();
}

static void view_update_typing(ConvView *v)
{
    char *title, *text = NULL;

    if(view_is_chat(v))
        return;
    title = view_title(v);
    switch(purple_conv_im_get_typing_state(PURPLE_CONV_IM(v->conv)))
    {
        case PURPLE_TYPING:
            text = g_strdup_printf("%s is typing...", title);
            if(v->activity <= ACT_TYPING && !view_visible(v))
                view_set_activity(v, ACT_TYPING);
            break;
        case PURPLE_TYPED:
            text = g_strdup_printf("%s has stopped typing", title);
            if(v->activity == ACT_TYPING)
                view_set_activity(v, ACT_NONE);
            break;
        default:
            if(v->activity == ACT_TYPING)
                view_set_activity(v, ACT_NONE);
            break;
    }
    dw_window_set_text(v->typing, text ? text : "");
    g_free(text);
    g_free(title);
}

/* ============================================================ chat users */

static int compare_cb(gconstpointer a, gconstpointer b)
{
    const PurpleConvChatBuddy *x = *(PurpleConvChatBuddy *const *)a;
    const PurpleConvChatBuddy *y = *(PurpleConvChatBuddy *const *)b;
    int fx = x->flags & (PURPLE_CBFLAGS_FOUNDER | PURPLE_CBFLAGS_OP | PURPLE_CBFLAGS_HALFOP | PURPLE_CBFLAGS_VOICE);
    int fy = y->flags & (PURPLE_CBFLAGS_FOUNDER | PURPLE_CBFLAGS_OP | PURPLE_CBFLAGS_HALFOP | PURPLE_CBFLAGS_VOICE);

    if(fx != fy)
        return fy - fx;
    return purple_utf8_strcasecmp(x->alias && *x->alias ? x->alias : x->name,
                                  y->alias && *y->alias ? y->alias : y->name);
}

static void view_refresh_users(ConvView *v)
{
    GList *users, *l;
    GPtrArray *sorted;
    void *alloc;
    guint z;
    char buf[64];

    v->users_pending = FALSE;
    if(!v->users)
        return;
    dw_container_clear(v->users, TRUE);
    users = purple_conv_chat_get_users(PURPLE_CONV_CHAT(v->conv));
    sorted = g_ptr_array_new();
    for(l = users; l; l = l->next)
        g_ptr_array_add(sorted, l->data);
    g_ptr_array_sort(sorted, compare_cb);
    if(sorted->len)
    {
        alloc = dw_container_alloc(v->users, (int)sorted->len);
        for(z = 0; z < sorted->len; z++)
        {
            PurpleConvChatBuddy *cb = g_ptr_array_index(sorted, z);
            const char *prefix = "";
            char *shown;

            if(cb->flags & PURPLE_CBFLAGS_FOUNDER)
                prefix = "~";
            else if(cb->flags & PURPLE_CBFLAGS_OP)
                prefix = "@";
            else if(cb->flags & PURPLE_CBFLAGS_HALFOP)
                prefix = "%";
            else if(cb->flags & PURPLE_CBFLAGS_VOICE)
                prefix = "+";
            shown = g_strdup_printf("%s%s%s%s", prefix,
                                    cb->alias && *cb->alias ? cb->alias : cb->name,
                                    (cb->flags & PURPLE_CBFLAGS_AWAY) ? " (away)" : "",
                                    (cb->flags & PURPLE_CBFLAGS_TYPING) ? " ..." : "");
            dw_container_set_item(v->users, alloc, 0, (int)z, &shown);
            /* The row title carries the real name for the callbacks. */
            dw_container_set_row_title(alloc, (int)z, cb->name);
            g_free(shown);
        }
        dw_container_insert(v->users, alloc, (int)sorted->len);
    }
    g_snprintf(buf, sizeof(buf), "%u user%s in room", sorted->len, sorted->len == 1 ? "" : "s");
    dw_window_set_text(v->user_count, buf);
    g_ptr_array_free(sorted, TRUE);
}

static void users_task(gpointer data)
{
    PurpleConversation *conv = data;
    ConvView *v;

    if(!dwp_conversation_exists(conv) || !(v = view_from_conv(conv)) || !v->users_pending)
        return;
    view_refresh_users(v);
}

static void view_schedule_users(ConvView *v)
{
    if(!v->users || v->users_pending)
        return;
    v->users_pending = TRUE;
    dwp_defer(users_task, v->conv);
}

static char *chat_real_name(ConvView *v, const char *who)
{
    PurpleConnection *gc = purple_conversation_get_gc(v->conv);
    PurplePluginProtocolInfo *info = dwp_prpl_info(gc);

    if(info && info->get_cb_real_name)
    {
        char *real = info->get_cb_real_name(gc, purple_conv_chat_get_id(PURPLE_CONV_CHAT(v->conv)), who);

        if(real)
            return real;
    }
    return g_strdup(who);
}

static void user_send_im(gpointer conv, gpointer b, const char *who)
{
    ConvView *v;
    char *real;

    (void)b;
    if(!dwp_conversation_exists(conv) || !(v = view_from_conv(conv)) || !who)
        return;
    real = chat_real_name(v, who);
    dwp_open_im(purple_conversation_get_account(v->conv), real);
    g_free(real);
}

static void user_get_info(gpointer conv, gpointer b, const char *who)
{
    ConvView *v;
    PurpleConnection *gc;
    PurplePluginProtocolInfo *info;

    (void)b;
    if(!dwp_conversation_exists(conv) || !(v = view_from_conv(conv)) || !who)
        return;
    gc = purple_conversation_get_gc(v->conv);
    info = dwp_prpl_info(gc);
    if(!gc)
        return;
    if(info && info->get_cb_info)
        info->get_cb_info(gc, purple_conv_chat_get_id(PURPLE_CONV_CHAT(v->conv)), who);
    else
    {
        char *real = chat_real_name(v, who);

        serv_get_info(gc, real);
        g_free(real);
    }
}

static void user_ignore(gpointer conv, gpointer b, const char *who)
{
    PurpleConvChat *chat;

    (void)b;
    if(!dwp_conversation_exists(conv) || !who)
        return;
    chat = PURPLE_CONV_CHAT((PurpleConversation *)conv);
    if(purple_conv_chat_is_user_ignored(chat, who))
        purple_conv_chat_unignore(chat, who);
    else
        purple_conv_chat_ignore(chat, who);
    if(view_from_conv(conv))
        view_schedule_users(view_from_conv(conv));
}

static void user_insert_nick(gpointer conv, gpointer b, const char *who)
{
    ConvView *v;
    char *text, *next;

    (void)b;
    if(!dwp_conversation_exists(conv) || !(v = view_from_conv(conv)) || !who)
        return;
    text = dwp_mle_get_text(v->input);
    next = *text ? g_strdup_printf("%s%s ", text, who) : g_strdup_printf("%s: ", who);
    dw_mle_set_cursor(v->input, (int)dwp_mle_set_text(v->input, next));
    dw_window_set_focus(v->input);
    g_free(next);
    g_free(text);
}

static int DWSIGNAL users_enter(HWND w, char *text, void *data, void *itemdata)
{
    ConvView *v = data;

    (void)w;
    (void)itemdata;
    if(view_alive(v) && text)
        user_send_im(v->conv, NULL, text);
    return TRUE;
}

static int DWSIGNAL users_context(HWND w, char *text, int x, int y, void *data, void *itemdata)
{
    ConvView *v = data;
    HMENUI menu;
    gboolean ignored;

    (void)w;
    (void)itemdata;
    if(!view_alive(v) || !text || !*text)
        return TRUE;
    dwp_actions_reset();
    menu = dw_menu_new(0);
    ignored = purple_conv_chat_is_user_ignored(PURPLE_CONV_CHAT(v->conv), text);
    dwp_menu_add(menu, "Send ~IM", user_send_im, v->conv, NULL, text);
    dwp_menu_add(menu, "Get ~Info", user_get_info, v->conv, NULL, text);
    dwp_menu_add(menu, "Insert ~Nick", user_insert_nick, v->conv, NULL, text);
    dwp_menu_add(menu, ignored ? "Un~ignore" : "~Ignore", user_ignore, v->conv, NULL, text);
    dw_menu_popup(&menu, cw.window, x, y);
    return TRUE;
}

/* ================================================================ input */

static void add_sent_history(ConvView *v, const char *text)
{
    if(!v->sent)
        v->sent = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(v->sent, g_strdup(text));
    if(v->sent->len > INPUT_HISTORY)
        g_ptr_array_remove_index(v->sent, 0);
    v->sent_pos = (int)v->sent->len;
}

static char *escape_input(const char *text)
{
    char *escaped = g_markup_escape_text(text, -1);
    char **parts = g_strsplit(escaped, "\n", -1);
    char *html = g_strjoinv("<br>", parts);

    g_strfreev(parts);
    g_free(escaped);
    return html;
}

static void view_send(ConvView *v)
{
    char *text = dwp_mle_get_text(v->input);
    char *trimmed = g_strstrip(g_strdup(text));
    PurpleConnection *gc;
    PurplePluginProtocolInfo *info;
    PurpleConversation *conv = v->conv;
    gboolean is_command;
    char *html;

    if(!*trimmed)
    {
        g_free(trimmed);
        g_free(text);
        return;
    }
    g_free(trimmed);
    add_sent_history(v, text);
    dw_mle_clear(v->input);
    gc = purple_conversation_get_gc(conv);
    info = dwp_prpl_info(gc);
    is_command = text[0] == '/' && text[1] != '/';

    if(is_command)
    {
        char *markup = escape_input(text + 1);
        char *error = NULL;
        PurpleCmdStatus status = purple_cmd_do_command(conv, text + 1, markup, &error);
        gboolean handled = TRUE;

        g_free(markup);
        switch(status)
        {
            case PURPLE_CMD_STATUS_OK:
                break;
            case PURPLE_CMD_STATUS_NOT_FOUND:
                if(info && (info->options & OPT_PROTO_SLASH_COMMANDS_NATIVE))
                    handled = FALSE;
                else
                    system_message(v, "No such command.", TRUE);
                break;
            case PURPLE_CMD_STATUS_WRONG_ARGS:
                system_message(v, "Syntax error: wrong number of arguments for that command.", TRUE);
                break;
            case PURPLE_CMD_STATUS_FAILED:
                system_message(v, error ? error : "The command failed for an unknown reason.", TRUE);
                break;
            case PURPLE_CMD_STATUS_WRONG_TYPE:
                system_message(v, view_is_chat(v) ? "That command only works in IMs, not chats."
                                                  : "That command only works in chats, not IMs.", TRUE);
                break;
            case PURPLE_CMD_STATUS_WRONG_PRPL:
                system_message(v, "That command doesn't work on this protocol.", TRUE);
                break;
        }
        g_free(error);
        /* A command such as /part may have destroyed the conversation. */
        if(handled || !dwp_conversation_exists(conv))
        {
            g_free(text);
            return;
        }
        html = escape_input(text);
    }
    else if(text[0] == '/' && text[1] == '/')
        html = escape_input(text + 1);
    else
        html = escape_input(text);

    if(!gc || !dwp_connection_exists(gc))
        system_message(v, "Message not sent: you are not connected.", TRUE);
    else
    {
        purple_idle_touch();
        if(view_is_chat(v))
            purple_conv_chat_send(PURPLE_CONV_CHAT(conv), html);
        else
        {
            PurpleConvIm *im = PURPLE_CONV_IM(conv);

            purple_conv_im_stop_send_typed_timeout(im);
            purple_conv_im_set_type_again(im, 0);
            purple_conv_im_send(im, html);
        }
    }
    g_free(html);
    g_free(text);
}

static void typing_task(gpointer data)
{
    PurpleConversation *conv = data;
    ConvView *v;
    PurpleConnection *gc;
    PurpleConvIm *im;
    const char *name;
    char *text;

    if(!dwp_conversation_exists(conv) || !(v = view_from_conv(conv)))
        return;
    v->typing_pending = FALSE;
    if(view_is_chat(v) || !purple_prefs_get_bool("/purple/conversations/im/send_typing"))
        return;
    if(!(gc = purple_conversation_get_gc(conv)))
        return;
    im = PURPLE_CONV_IM(conv);
    name = purple_conversation_get_name(conv);
    text = dwp_mle_get_text(v->input);
    if(!*text)
    {
        purple_conv_im_stop_send_typed_timeout(im);
        purple_conv_im_set_type_again(im, 0);
        serv_send_typing(gc, name, PURPLE_NOT_TYPING);
        g_free(text);
        return;
    }
    g_free(text);
    if(purple_conv_im_get_send_typed_timeout(im))
        purple_conv_im_stop_send_typed_timeout(im);
    purple_conv_im_start_send_typed_timeout(im);
    if(time(NULL) > purple_conv_im_get_type_again(im))
    {
        unsigned int timeout = serv_send_typing(gc, name, PURPLE_TYPING);

        purple_conv_im_set_type_again(im, timeout);
    }
}

static void complete_nick(ConvView *v)
{
    char *text = dwp_mle_get_text(v->input);
    char *word_start, *word, *completion = NULL;
    GList *users, *l;
    GPtrArray *matches = g_ptr_array_new();
    gboolean at_start;

    word_start = text + strlen(text);
    while(word_start > text && word_start[-1] != ' ' && word_start[-1] != '\n' && word_start[-1] != '\t')
        word_start--;
    word = word_start;
    at_start = (word_start == text);
    if(!*word)
        goto out;
    users = purple_conv_chat_get_users(PURPLE_CONV_CHAT(v->conv));
    for(l = users; l; l = l->next)
    {
        PurpleConvChatBuddy *cb = l->data;
        const char *name = cb->alias && *cb->alias ? cb->alias : cb->name;

        if(name && g_ascii_strncasecmp(name, word, strlen(word)) == 0)
            g_ptr_array_add(matches, (gpointer)name);
    }
    if(matches->len == 1)
        completion = g_strdup_printf("%s%s", (char *)g_ptr_array_index(matches, 0), at_start ? ": " : " ");
    else if(matches->len > 1)
    {
        const char *first = g_ptr_array_index(matches, 0);
        size_t len = strlen(first), z;
        GString *list = g_string_new("Matches: ");

        for(z = 1; z < matches->len; z++)
        {
            const char *m = g_ptr_array_index(matches, z);
            size_t i = 0;

            while(i < len && m[i] && g_ascii_tolower(m[i]) == g_ascii_tolower(first[i]))
                i++;
            len = i;
        }
        if(len >= strlen(word))
            completion = g_strndup(first, len);
        for(z = 0; z < matches->len && z < 12; z++)
            g_string_append_printf(list, "%s%s", z ? ", " : "", (char *)g_ptr_array_index(matches, z));
        if(matches->len > 12)
            g_string_append(list, ", ...");
        dw_window_set_text(v->typing, list->str);
        g_string_free(list, TRUE);
    }
    if(completion)
    {
        GString *next = g_string_new_len(text, word_start - text);

        g_string_append(next, completion);
        dw_mle_set_cursor(v->input, (int)dwp_mle_set_text(v->input, next->str));
        g_string_free(next, TRUE);
        g_free(completion);
    }
out:
    g_ptr_array_free(matches, TRUE);
    g_free(text);
}

static void history_recall(ConvView *v, int dir)
{
    const char *text;

    if(!v->sent || !v->sent->len)
        return;
    v->sent_pos += dir;
    if(v->sent_pos < 0)
        v->sent_pos = 0;
    if(v->sent_pos >= (int)v->sent->len)
    {
        v->sent_pos = (int)v->sent->len;
        dw_mle_clear(v->input);
        return;
    }
    text = g_ptr_array_index(v->sent, v->sent_pos);
    dw_mle_set_cursor(v->input, (int)dwp_mle_set_text(v->input, text));
}

static int DWSIGNAL input_key(HWND w, char ch, int vk, int state, void *data, char *utf8)
{
    ConvView *v = data;
    gboolean enter = (vk == VK_RETURN) || ch == '\r' || ch == '\n';

    (void)w;
    (void)utf8;
#ifdef VK_ENTER
    if(vk == VK_ENTER)
        enter = TRUE;
#endif
    if(!view_alive(v))
        return FALSE;
    dwp_note_activity();
    if(enter && !(state & KC_SHIFT))
    {
        view_send(v);
        return TRUE;
    }
    if((vk == VK_TAB || ch == '\t') && view_is_chat(v) && !(state & (KC_SHIFT | KC_CTRL | KC_ALT)))
    {
        complete_nick(v);
        return TRUE;
    }
    if((state & KC_CTRL) && (vk == VK_UP || vk == VK_DOWN))
    {
        history_recall(v, vk == VK_UP ? -1 : 1);
        return TRUE;
    }
    if(v->activity)
        view_set_activity(v, ACT_NONE);
    /* The text changes after this handler; look at it from the main loop. */
    if(!view_is_chat(v) && !v->typing_pending)
    {
        v->typing_pending = TRUE;
        dwp_defer(typing_task, v->conv);
    }
    return FALSE;
}

static int DWSIGNAL send_clicked(HWND w, void *data)
{
    ConvView *v = data;

    (void)w;
    if(view_alive(v))
    {
        view_send(v);
        dw_window_set_focus(v->input);
    }
    return TRUE;
}

static int DWSIGNAL close_clicked(HWND w, void *data)
{
    ConvView *v = data;

    (void)w;
    if(view_alive(v))
        purple_conversation_destroy(v->conv);
    return TRUE;
}

/* ================================================================== views */

static ConvView *view_new(PurpleConversation *conv)
{
    ConvView *v = g_new0(ConvView, 1);
    HWND header, vbox, row, histbox;
    unsigned long flags[1] = { DW_CFA_STRING | DW_CFA_LEFT | DW_CFA_HORZSEPARATOR };
    char *titles[1] = { "Users" };

    v->conv = conv;
    g_queue_init(&v->lines);
    load_display_options(v);

    v->box = dw_box_new(DW_VERT, 2);
    header = dw_box_new(DW_HORZ, 0);
    dw_box_pack_start(v->box, header, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 1);
    vbox = dw_box_new(DW_VERT, 0);
    dw_box_pack_start(header, vbox, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 1);
    v->title = dw_text_new("", 0);
    dw_box_pack_start(vbox, v->title, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 1);
    v->subtitle = dw_text_new("", 0);
    dw_box_pack_start(vbox, v->subtitle, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 1);
    dwp_button(header, "Close", DW_SIGNAL_FUNC(close_clicked), v);

    v->history = dw_mle_new(0);
    dw_mle_set_editable(v->history, FALSE);
    dw_mle_set_word_wrap(v->history, TRUE);

    if(purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_CHAT)
    {
        HWND userbox = dw_box_new(DW_VERT, 0), split;

        v->user_count = dw_text_new("", 0);
        dw_box_pack_start(userbox, v->user_count, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 1);
        v->users = dw_container_new(0, FALSE);
        dw_container_setup(v->users, flags, titles, 1, 0);
        dw_box_pack_start(userbox, v->users, 120, 200, TRUE, TRUE, 1);
        dw_signal_connect(v->users, DW_SIGNAL_ITEM_ENTER, DW_SIGNAL_FUNC(users_enter), v);
        dw_signal_connect(v->users, DW_SIGNAL_ITEM_CONTEXT, DW_SIGNAL_FUNC(users_context), v);
        histbox = dw_box_new(DW_VERT, 0);
        dw_box_pack_start(histbox, v->history, 300, 200, TRUE, TRUE, 0);
        split = dw_splitbar_new(DW_HORZ, histbox, userbox, 0);
        dw_box_pack_start(v->box, split, 400, 250, TRUE, TRUE, 1);
        dw_splitbar_set(split, 76.0f);
    }
    else
        dw_box_pack_start(v->box, v->history, 400, 250, TRUE, TRUE, 1);

    v->typing = dw_text_new("", 0);
    dw_box_pack_start(v->box, v->typing, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 1);

    row = dw_box_new(DW_HORZ, 0);
    dw_box_pack_start(v->box, row, DW_SIZE_AUTO, 70, TRUE, FALSE, 1);
    v->input = dw_mle_new(0);
    dw_mle_set_word_wrap(v->input, TRUE);
    dw_box_pack_start(row, v->input, 300, 64, TRUE, TRUE, 1);
    dwp_button(row, "Send", DW_SIGNAL_FUNC(send_clicked), v);
    dw_signal_connect(v->input, DW_SIGNAL_KEY_PRESS, DW_SIGNAL_FUNC(input_key), v);

    v->page = dw_notebook_page_new(cw.notebook, 0, FALSE);
    dw_notebook_pack(cw.notebook, v->page, v->box);
    cw.views = g_list_append(cw.views, v);
    conv->ui_data = v;
    view_update_header(v);
    if(v->users)
        view_refresh_users(v);
    return v;
}

static void view_free(ConvView *v)
{
    clear_lines(v);
    if(v->sent)
        g_ptr_array_free(v->sent, TRUE);
    g_free(v);
}

/* ================================================================ window */

static void window_update_title(void)
{
    ConvView *v = current_view();
    int unread = dwp_conv_unread_count();
    char *title;

    if(!cw.window)
        return;
    if(v)
    {
        char *t = view_title(v);

        title = unread ? g_strdup_printf("(%d) %s - %s", unread, t, DWP_NAME)
                       : g_strdup_printf("%s - %s", t, DWP_NAME);
        g_free(t);
    }
    else
        title = g_strdup_printf("Conversations - %s", DWP_NAME);
    dw_window_set_text(cw.window, title);
    g_free(title);
}

static void sync_option_checks(ConvView *v)
{
    if(!cw.options_menu)
        return;
    dwp_menu_set_state(cw.options_menu, DWP_MID_CONV_REACTIONS,
                       v && v->show_reactions ? DW_MIS_CHECKED : DW_MIS_UNCHECKED);
    dwp_menu_set_state(cw.options_menu, DWP_MID_CONV_EMOJI,
                       v && v->emoji_codes ? DW_MIS_CHECKED : DW_MIS_UNCHECKED);
    dwp_menu_set_state(cw.options_menu, DWP_MID_CONV_TIMESTAMPS,
                       purple_prefs_get_bool(DWP_PREF_TIMESTAMPS) ? DW_MIS_CHECKED : DW_MIS_UNCHECKED);
}

static void view_activated(ConvView *v)
{
    if(!v)
        return;
    if(v->activity)
        view_set_activity(v, ACT_NONE);
    sync_option_checks(v);
    window_update_title();
    dw_window_set_focus(v->input);
}

static int DWSIGNAL page_switched(HWND w, unsigned long page, void *data)
{
    GList *l;

    (void)w;
    (void)data;
    for(l = cw.views; l; l = l->next)
    {
        ConvView *v = l->data;

        if(v->page == page)
        {
            cw.current = page;
            view_activated(v);
            return FALSE;
        }
    }
    return FALSE;
}

static void close_all_task(gpointer unused)
{
    GList *convs = NULL, *l;

    (void)unused;
    for(l = cw.views; l; l = l->next)
        convs = g_list_append(convs, ((ConvView *)l->data)->conv);
    for(l = convs; l; l = l->next)
        if(dwp_conversation_exists(l->data))
            purple_conversation_destroy(l->data);
    g_list_free(convs);
}

static int DWSIGNAL conv_window_delete(HWND w, void *data)
{
    (void)w;
    (void)data;
    dwp_conv_save_geometry();
    dw_window_hide(cw.window);
    cw.visible = FALSE;
    /* Closing the window ends its conversations, as in Pidgin. */
    dwp_defer(close_all_task, NULL);
    return TRUE;
}

/* ------------------------------------------------------------ menu items */

static ConvView *menu_view(void)
{
    ConvView *v = current_view();

    return view_alive(v) ? v : NULL;
}

static void cm_send_file(gpointer a, gpointer b, const char *s)
{
    ConvView *v = menu_view();
    PurpleConnection *gc;
    PurplePluginProtocolInfo *info;

    (void)a; (void)b; (void)s;
    if(!v)
        return;
    gc = purple_conversation_get_gc(v->conv);
    info = dwp_prpl_info(gc);
    if(view_is_chat(v) || !info || !info->send_file)
    {
        system_message(v, "This conversation cannot receive files.", TRUE);
        return;
    }
    serv_send_file(gc, purple_conversation_get_name(v->conv), NULL);
}

static void cm_send_image(gpointer a, gpointer b, const char *s)
{
    ConvView *v = menu_view();
    PurpleConversation *conv;
    PurpleConnection *gc;
    char *path, *data = NULL, *name, *html;
    gsize len = 0;
    int id;

    (void)a; (void)b; (void)s;
    if(!v)
        return;
    conv = v->conv;
    path = dw_file_browse("Send Image", NULL, NULL, DW_FILE_OPEN);
    if(!path)
        return;
    /* The file dialog ran a nested loop: re-check everything. */
    if(!dwp_conversation_exists(conv) || !(v = view_from_conv(conv)))
    {
        dw_free(path);
        return;
    }
    gc = purple_conversation_get_gc(conv);
    if(!gc || !(gc->flags & PURPLE_CONNECTION_HTML) || (gc->flags & PURPLE_CONNECTION_NO_IMAGES))
    {
        PurplePluginProtocolInfo *info = dwp_prpl_info(gc);

        if(gc && !view_is_chat(v) && info && info->send_file)
            serv_send_file(gc, purple_conversation_get_name(conv), path);
        else
            system_message(v, "Images cannot be sent in this conversation.", TRUE);
        dw_free(path);
        return;
    }
    if(!g_file_get_contents(path, &data, &len, NULL) || !len)
    {
        system_message(v, "The image could not be read.", TRUE);
        dw_free(path);
        return;
    }
    name = g_path_get_basename(path);
    dw_free(path);
    id = purple_imgstore_add_with_id(data, len, name);    /* takes data */
    g_free(name);
    if(!id)
        return;
    html = g_strdup_printf("<img id=\"%d\">", id);
    if(view_is_chat(v))
        purple_conv_chat_send(PURPLE_CONV_CHAT(conv), html);
    else
        purple_conv_im_send(PURPLE_CONV_IM(conv), html);
    purple_imgstore_unref_by_id(id);
    g_free(html);
}

static void insert_emoji(gpointer conv, gpointer b, const char *code)
{
    ConvView *v;
    char *text, *next;

    (void)b;
    if(!dwp_conversation_exists(conv) || !(v = view_from_conv(conv)) || !code)
        return;
    text = dwp_mle_get_text(v->input);
    next = g_strdup_printf("%s%s%s ", text, (*text && text[strlen(text) - 1] != ' ') ? " " : "", code);
    dw_mle_set_cursor(v->input, (int)dwp_mle_set_text(v->input, next));
    dw_window_set_focus(v->input);
    g_free(next);
    g_free(text);
}

static void cm_emoji(gpointer a, gpointer b, const char *s)
{
    ConvView *v = menu_view();
    const char *const *codes = dwp_emoji_picker_codes();
    HMENUI menu, sub = 0;
    int z;

    (void)a; (void)b; (void)s;
    if(!v)
        return;
    dwp_actions_reset();
    menu = dw_menu_new(0);
    for(z = 0; codes[z]; z++)
    {
        char *code = g_strdup_printf(":%s:", codes[z]);
        const char *glyph = dwp_emoji_lookup(codes[z]);
        char *label = (glyph && v->emoji_codes) ? g_strdup_printf("%s  %s", glyph, code) : g_strdup(code);

        if(z % 20 == 0)
        {
            char *title = g_strdup_printf("Emoji %d-%d", z + 1, z + 20);

            sub = dw_menu_new(0);
            dw_menu_append_item(menu, title, DW_MENU_POPUP, 0, TRUE, FALSE, sub);
            g_free(title);
        }
        dwp_menu_add(sub, label, insert_emoji, v->conv, NULL, code);
        g_free(label);
        g_free(code);
    }
    dwp_menu_popup_here(&menu, cw.window);
}

static void cm_get_info(gpointer a, gpointer b, const char *s)
{
    ConvView *v = menu_view();
    PurpleConnection *gc;

    (void)a; (void)b; (void)s;
    if(!v || view_is_chat(v) || !(gc = purple_conversation_get_gc(v->conv)))
        return;
    serv_get_info(gc, purple_conversation_get_name(v->conv));
}

static void cm_add_buddy(gpointer a, gpointer b, const char *s)
{
    ConvView *v = menu_view();

    (void)a; (void)b; (void)s;
    if(!v)
        return;
    if(view_is_chat(v))
        dwp_show_add_chat(purple_conversation_get_account(v->conv), NULL, NULL,
                          purple_conversation_get_name(v->conv));
    else
        dwp_show_add_buddy(purple_conversation_get_account(v->conv),
                           purple_conversation_get_name(v->conv), NULL, NULL);
}

static void cm_block(gpointer a, gpointer b, const char *s)
{
    ConvView *v = menu_view();
    PurpleAccount *account;
    const char *name;

    (void)a; (void)b; (void)s;
    if(!v || view_is_chat(v))
        return;
    account = purple_conversation_get_account(v->conv);
    name = purple_conversation_get_name(v->conv);
    dwp_privacy_set_blocked(account, name, !dwp_privacy_is_blocked(account, name));
    system_message(v, dwp_privacy_is_blocked(account, name) ? "Blocked." : "Unblocked.", FALSE);
}

static void invite_done(gpointer conv, const char *who)
{
    PurpleConnection *gc;
    char *name;

    if(!dwp_conversation_exists(conv) || !(gc = purple_conversation_get_gc(conv)))
        return;
    name = g_strstrip(g_strdup(who ? who : ""));
    if(*name)
        serv_chat_invite(gc, purple_conv_chat_get_id(PURPLE_CONV_CHAT((PurpleConversation *)conv)), NULL, name);
    g_free(name);
}

static void cm_invite(gpointer a, gpointer b, const char *s)
{
    ConvView *v = menu_view();

    (void)a; (void)b; (void)s;
    if(!v || !view_is_chat(v))
        return;
    dwp_prompt("Invite to Chat", "Buddy to invite:", "", FALSE, invite_done, v->conv);
}

static void cm_pounce(gpointer a, gpointer b, const char *s)
{
    ConvView *v = menu_view();

    (void)a; (void)b; (void)s;
    if(v && !view_is_chat(v))
        dwp_pounce_edit(NULL, purple_conversation_get_account(v->conv), purple_conversation_get_name(v->conv));
}

static void cm_clear(gpointer a, gpointer b, const char *s)
{
    ConvView *v = menu_view();

    (void)a; (void)b; (void)s;
    if(!v)
        return;
    purple_conversation_clear_message_history(v->conv);
    clear_lines(v);
    view_rerender(v);
}

static void cm_close(gpointer a, gpointer b, const char *s)
{
    ConvView *v = menu_view();

    (void)a; (void)b; (void)s;
    if(v)
        purple_conversation_destroy(v->conv);
}

static void cm_reactions(gpointer a, gpointer b, const char *s)
{
    ConvView *v = menu_view();

    (void)a; (void)b; (void)s;
    if(!v)
        return;
    v->show_reactions = !v->show_reactions;
    save_display_option(v, reactions_key, v->show_reactions);
    view_rerender(v);
    sync_option_checks(v);
    if(!v->show_reactions && v->hidden_reactions)
    {
        char buf[96];

        g_snprintf(buf, sizeof(buf), "%d reaction notice(s) hidden.", v->hidden_reactions);
        dw_window_set_text(v->typing, buf);
    }
}

static void cm_emoji_codes(gpointer a, gpointer b, const char *s)
{
    ConvView *v = menu_view();

    (void)a; (void)b; (void)s;
    if(!v)
        return;
    v->emoji_codes = !v->emoji_codes;
    save_display_option(v, emoji_key, v->emoji_codes);
    view_rerender(v);
    sync_option_checks(v);
}

static void cm_timestamps(gpointer a, gpointer b, const char *s)
{
    GList *l;

    (void)a; (void)b; (void)s;
    purple_prefs_set_bool(DWP_PREF_TIMESTAMPS, !purple_prefs_get_bool(DWP_PREF_TIMESTAMPS));
    for(l = cw.views; l; l = l->next)
        view_rerender(l->data);
    sync_option_checks(menu_view());
}

static void ensure_window(void)
{
    HMENUI menu;
    int w, h;

    if(cw.window)
        return;
    cw.window = dwp_window_new("Conversations", 0, 0);
    cw.menubar = dw_menubar_new(cw.window);

    menu = dw_menu_new(0);
    dwp_menu_add_static(menu, "Send ~File...", 0, FALSE, cm_send_file, NULL);
    dwp_menu_add_static(menu, "Send I~mage...", 0, FALSE, cm_send_image, NULL);
    dwp_menu_add_static(menu, "Insert ~Emoji...", 0, FALSE, cm_emoji, NULL);
    dwp_menu_separator(menu);
    dwp_menu_add_static(menu, "Get ~Info", 0, FALSE, cm_get_info, NULL);
    dwp_menu_add_static(menu, "~Add Buddy/Chat...", 0, FALSE, cm_add_buddy, NULL);
    dwp_menu_add_static(menu, "~Block/Unblock", 0, FALSE, cm_block, NULL);
    dwp_menu_add_static(menu, "In~vite...", 0, FALSE, cm_invite, NULL);
    dwp_menu_add_static(menu, "Buddy ~Pounce...", 0, FALSE, cm_pounce, NULL);
    dwp_menu_separator(menu);
    dwp_menu_add_static(menu, "C~lear Scrollback", 0, FALSE, cm_clear, NULL);
    dwp_menu_add_static(menu, "~Close Tab", 0, FALSE, cm_close, NULL);
    dw_menu_append_item(cw.menubar, "~Conversation", DW_MENU_AUTO, 0, TRUE, FALSE, menu);

    menu = dw_menu_new(0);
    cw.options_menu = menu;
    dwp_menu_add_static(menu, "Show ~Reactions", DWP_MID_CONV_REACTIONS, TRUE, cm_reactions, NULL);
    dwp_menu_add_static(menu, "Convert ~Emoji Codes", DWP_MID_CONV_EMOJI, TRUE, cm_emoji_codes, NULL);
    dwp_menu_add_static(menu, "Show ~Timestamps", DWP_MID_CONV_TIMESTAMPS, TRUE, cm_timestamps, NULL);
    dw_menu_append_item(cw.menubar, "~Options", DW_MENU_AUTO, 0, TRUE, FALSE, menu);

    cw.notebook = dw_notebook_new(0, TRUE);
    dw_box_pack_start(cw.window, cw.notebook, 0, 0, TRUE, TRUE, 0);
    dw_signal_connect(cw.notebook, DW_SIGNAL_SWITCH_PAGE, DW_SIGNAL_FUNC(page_switched), NULL);
    dw_signal_connect(cw.window, DW_SIGNAL_DELETE, DW_SIGNAL_FUNC(conv_window_delete), NULL);
    w = purple_prefs_get_int(DWP_PREF_CONV_W);
    h = purple_prefs_get_int(DWP_PREF_CONV_H);
    dw_window_set_size(cw.window, w > 200 ? w : 680, h > 150 ? h : 480);
}

static void window_show(gboolean raise)
{
    ensure_window();
    if(!cw.visible)
    {
        dw_window_show(cw.window);
        cw.visible = TRUE;
    }
    if(raise)
        dw_window_raise(cw.window);
}

void dwp_conv_save_geometry(void)
{
    long x = 0, y = 0;
    unsigned long w = 0, h = 0;

    if(!cw.window || !cw.visible)
        return;
    dw_window_get_pos_size(cw.window, &x, &y, &w, &h);
    if(w > 200 && h > 150)
    {
        purple_prefs_set_int(DWP_PREF_CONV_W, (int)w);
        purple_prefs_set_int(DWP_PREF_CONV_H, (int)h);
    }
}

int dwp_conv_unread_count(void)
{
    GList *l;
    int n = 0;

    for(l = cw.views; l; l = l->next)
        if(((ConvView *)l->data)->activity >= ACT_MESSAGE)
            n++;
    return n;
}

gboolean dwp_conv_present_first_unread(void)
{
    GList *l;

    for(l = cw.views; l; l = l->next)
    {
        ConvView *v = l->data;

        if(v->activity >= ACT_MESSAGE)
        {
            purple_conversation_present(v->conv);
            return TRUE;
        }
    }
    return FALSE;
}

/* ================================================================ ui ops */

static void conv_create(PurpleConversation *conv)
{
    ensure_window();
    view_new(conv);
    /* Show without stealing focus; present() raises. */
    window_show(purple_prefs_get_bool(DWP_PREF_RAISE_NEW));
    if(g_list_length(cw.views) == 1)
        view_activated(conv->ui_data);
    window_update_title();
}

static void page_destroy_task(gpointer data)
{
    ConvView *v = data;

    dw_notebook_page_destroy(cw.notebook, v->page);
    view_free(v);
}

static void conv_destroy(PurpleConversation *conv)
{
    ConvView *v = view_from_conv(conv);

    if(!v)
        return;
    conv->ui_data = NULL;
    cw.views = g_list_remove(cw.views, v);
    /* The page may contain the widget whose callback got us here (Close
     * button, Enter in the input box running /part...): destroy it from
     * the main loop.  Widget callbacks check view_alive() first. */
    dwp_defer(page_destroy_task, v);
    if(!cw.views && cw.window)
    {
        dwp_conv_save_geometry();
        dw_window_hide(cw.window);
        cw.visible = FALSE;
    }
    else
        view_activated(current_view());
    window_update_title();
    dwp_tray_update();
}

static void conv_write(PurpleConversation *conv, const char *name, const char *alias,
                       const char *message, PurpleMessageFlags flags, time_t mtime)
{
    ConvView *v = view_from_conv(conv);

    if(v)
        view_write(v, name, alias, message, flags, mtime);
}

static void conv_chat_add_users(PurpleConversation *conv, GList *cbuddies, gboolean new_arrivals)
{
    ConvView *v = view_from_conv(conv);

    (void)cbuddies;
    (void)new_arrivals;
    if(v)
        view_schedule_users(v);
}

static void conv_chat_rename_user(PurpleConversation *conv, const char *old_name,
                                  const char *new_name, const char *new_alias)
{
    ConvView *v = view_from_conv(conv);

    (void)old_name;
    (void)new_name;
    (void)new_alias;
    if(v)
        view_schedule_users(v);
}

static void conv_chat_remove_users(PurpleConversation *conv, GList *users)
{
    ConvView *v = view_from_conv(conv);

    (void)users;
    if(v)
        view_schedule_users(v);
}

static void conv_chat_update_user(PurpleConversation *conv, const char *user)
{
    ConvView *v = view_from_conv(conv);

    (void)user;
    if(v)
        view_schedule_users(v);
}

static void conv_present(PurpleConversation *conv)
{
    ConvView *v = view_from_conv(conv);

    if(!v)
        return;
    window_show(TRUE);
    dw_notebook_page_set(cw.notebook, v->page);
    cw.current = v->page;
    view_activated(v);
}

static gboolean conv_has_focus(PurpleConversation *conv)
{
    return view_visible(view_from_conv(conv));
}

PurpleConversationUiOps *dwp_conv_ui_ops(void)
{
    static PurpleConversationUiOps ops;
    static gboolean ready = FALSE;

    if(!ready)
    {
        memset(&ops, 0, sizeof(ops));
        ops.create_conversation = conv_create;
        ops.destroy_conversation = conv_destroy;
        ops.write_conv = conv_write;
        ops.chat_add_users = conv_chat_add_users;
        ops.chat_rename_user = conv_chat_rename_user;
        ops.chat_remove_users = conv_chat_remove_users;
        ops.chat_update_user = conv_chat_update_user;
        ops.present = conv_present;
        ops.has_focus = conv_has_focus;
        ready = TRUE;
    }
    return &ops;
}

/* ========================================================== signal hooks */

static void on_conversation_updated(PurpleConversation *conv, PurpleConvUpdateType type, gpointer data)
{
    ConvView *v = view_from_conv(conv);

    (void)data;
    if(!v)
        return;
    switch(type)
    {
        case PURPLE_CONV_UPDATE_TYPING:
            view_update_typing(v);
            break;
        case PURPLE_CONV_UPDATE_ADD:
        case PURPLE_CONV_UPDATE_REMOVE:
        case PURPLE_CONV_UPDATE_ACCOUNT:
        case PURPLE_CONV_UPDATE_TOPIC:
        case PURPLE_CONV_UPDATE_AWAY:
        case PURPLE_CONV_UPDATE_TITLE:
        case PURPLE_CONV_UPDATE_CHATLEFT:
            view_update_header(v);
            if(v->users)
                view_schedule_users(v);
            break;
        default:
            break;
    }
}

static void update_buddy_views(PurpleBuddy *buddy)
{
    PurpleConversation *conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM,
                                                                     purple_buddy_get_name(buddy),
                                                                     purple_buddy_get_account(buddy));
    ConvView *v = view_from_conv(conv);

    if(v)
        view_update_header(v);
}

static void on_buddy_changed(PurpleBuddy *buddy, gpointer data)
{
    (void)data;
    update_buddy_views(buddy);
}

static void on_buddy_status_changed(PurpleBuddy *buddy, PurpleStatus *old, PurpleStatus *now, gpointer data)
{
    (void)old;
    (void)now;
    (void)data;
    update_buddy_views(buddy);
}

static void on_connection_changed(PurpleConnection *gc, gpointer data)
{
    GList *l;

    (void)gc;
    (void)data;
    for(l = cw.views; l; l = l->next)
        view_update_header(l->data);
}

/* ------------------------------------------------------------- commands */

static PurpleCmdRet cmd_say(PurpleConversation *conv, const char *cmd, char **args, char **error, void *data)
{
    (void)cmd; (void)error; (void)data;
    if(purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_IM)
        purple_conv_im_send(PURPLE_CONV_IM(conv), args[0]);
    else if(purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_CHAT)
        purple_conv_chat_send(PURPLE_CONV_CHAT(conv), args[0]);
    return PURPLE_CMD_RET_OK;
}

static PurpleCmdRet cmd_me(PurpleConversation *conv, const char *cmd, char **args, char **error, void *data)
{
    char *tmp = g_strdup_printf("/me %s", args[0]);

    (void)cmd; (void)error; (void)data;
    if(purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_IM)
        purple_conv_im_send(PURPLE_CONV_IM(conv), tmp);
    else if(purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_CHAT)
        purple_conv_chat_send(PURPLE_CONV_CHAT(conv), tmp);
    g_free(tmp);
    return PURPLE_CMD_RET_OK;
}

static PurpleCmdRet cmd_clear(PurpleConversation *conv, const char *cmd, char **args, char **error, void *data)
{
    ConvView *v = view_from_conv(conv);

    (void)cmd; (void)args; (void)error; (void)data;
    purple_conversation_clear_message_history(conv);
    if(v)
    {
        clear_lines(v);
        view_rerender(v);
    }
    return PURPLE_CMD_RET_OK;
}

static PurpleCmdRet cmd_help(PurpleConversation *conv, const char *cmd, char **args, char **error, void *data)
{
    GList *list, *l;
    GString *s;

    (void)cmd; (void)error; (void)data;
    if(args[0] && *args[0])
    {
        list = purple_cmd_help(conv, args[0]);
        if(!list)
        {
            char *msg = g_strdup_printf("No such command: %s", args[0]);
            ConvView *v = view_from_conv(conv);

            if(v)
                system_message(v, msg, TRUE);
            g_free(msg);
            return PURPLE_CMD_RET_OK;
        }
        s = g_string_new(NULL);
        for(l = list; l; l = l->next)
            g_string_append_printf(s, "%s%s", s->len ? "\n" : "", (char *)l->data);
    }
    else
    {
        list = purple_cmd_list(conv);
        list = g_list_sort(list, (GCompareFunc)strcmp);
        s = g_string_new("Use \"/help <command>\" for help on a specific command.\nThe following commands are available in this context:\n");
        for(l = list; l; l = l->next)
            g_string_append_printf(s, "%s%s", l == list ? "" : ", ", (char *)l->data);
    }
    g_list_free(list);
    {
        ConvView *v = view_from_conv(conv);

        if(v)
            system_message(v, s->str, FALSE);
    }
    g_string_free(s, TRUE);
    return PURPLE_CMD_RET_OK;
}

void dwp_conv_init(void)
{
    void *convs = purple_conversations_get_handle();
    void *blist = purple_blist_get_handle();
    void *conns = purple_connections_get_handle();
    PurpleCmdFlag both = PURPLE_CMD_FLAG_IM | PURPLE_CMD_FLAG_CHAT;

    purple_signal_connect(convs, "conversation-updated", &conv_handle,
                          PURPLE_CALLBACK(on_conversation_updated), NULL);
    purple_signal_connect(blist, "buddy-status-changed", &conv_handle,
                          PURPLE_CALLBACK(on_buddy_status_changed), NULL);
    purple_signal_connect(blist, "buddy-signed-on", &conv_handle, PURPLE_CALLBACK(on_buddy_changed), NULL);
    purple_signal_connect(blist, "buddy-signed-off", &conv_handle, PURPLE_CALLBACK(on_buddy_changed), NULL);
    purple_signal_connect(conns, "signed-on", &conv_handle, PURPLE_CALLBACK(on_connection_changed), NULL);
    purple_signal_connect(conns, "signed-off", &conv_handle, PURPLE_CALLBACK(on_connection_changed), NULL);

    cmd_ids[0] = purple_cmd_register("say", "S", PURPLE_CMD_P_DEFAULT, both, NULL,
                                     cmd_say, "say &lt;message&gt;:  Send a message normally as if you weren't using a command.", NULL);
    cmd_ids[1] = purple_cmd_register("me", "S", PURPLE_CMD_P_DEFAULT, both, NULL,
                                     cmd_me, "me &lt;action&gt;:  Send an IRC style action to a buddy or chat.", NULL);
    cmd_ids[2] = purple_cmd_register("clear", "", PURPLE_CMD_P_DEFAULT, both, NULL,
                                     cmd_clear, "clear: Clears the conversation scrollback.", NULL);
    cmd_ids[3] = purple_cmd_register("help", "w", PURPLE_CMD_P_DEFAULT,
                                     both | PURPLE_CMD_FLAG_ALLOW_WRONG_ARGS, NULL,
                                     cmd_help, "help &lt;command&gt;:  Help on a specific command.", NULL);
}
