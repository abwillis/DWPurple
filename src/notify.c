/*
 * DWPurple - libpurple notify UI: messages, mail, formatted text, user
 * info and search results.
 */
#include "dwpurple.h"

typedef struct _Note Note;

typedef struct {
    Note *note;
    PurpleNotifySearchButton *button;
} SearchButton;

struct _Note {
    PurpleNotifyType type;
    HWND window, list;
    gboolean closing, deleted;
    char *url;
    /* search results */
    PurpleAccount *account;
    PurpleNotifySearchResults *results;
    gpointer user_data;
    GPtrArray *buttons;
    int columns;
};

static GList *live = NULL;

static void note_free(gpointer p)
{
    Note *n = p;

    if(n->results)
        purple_notify_searchresults_free(n->results);
    if(n->buttons)
        g_ptr_array_free(n->buttons, TRUE);
    g_free(n->url);
    g_free(n);
}

static void close_from_core(Note *n)
{
    if(n->deleted)
        return;
    n->deleted = TRUE;
    n->closing = TRUE;
    live = g_list_remove(live, n);
    dwp_window_close_later(n->window, note_free, n);
}

/* User closed the window: tell libpurple, which runs its close callback. */
static void user_close(Note *n)
{
    if(n->closing)
        return;
    n->closing = TRUE;
    if(n->window)
        dw_window_hide(n->window);
    purple_notify_close(n->type, n);
    if(!n->deleted)
        close_from_core(n);
}

static int DWSIGNAL note_close(HWND w, void *data)
{
    (void)w;
    user_close(data);
    return TRUE;
}

static int DWSIGNAL note_open_url(HWND w, void *data)
{
    Note *n = data;

    (void)w;
    if(n->url)
        dwp_open_link(n->url);
    return TRUE;
}

static Note *note_new(PurpleNotifyType type, const char *title)
{
    Note *n = g_new0(Note, 1);

    n->type = type;
    n->window = dwp_window_new(title && *title ? title : DWP_NAME, 0, 0);
    live = g_list_append(live, n);
    return n;
}

static void add_text(HWND box, const char *text)
{
    /* Read-only text block: selectable, and never collapses the layout. */
    dwp_text_block(box, text, 400, 160);
}

/* body may contain HTML (formatted/user info) and is shown as text. */
static Note *simple_window(PurpleNotifyType type, const char *title, const char *primary,
                           const char *secondary, const char *body_html, const char *url)
{
    Note *n = note_new(type, title);
    HWND box = dw_box_new(DW_VERT, 4), buttons, closeb;
    gboolean big = FALSE;

    dw_box_pack_start(n->window, box, 0, 0, TRUE, TRUE, 0);
    if(primary && *primary)
        add_text(box, primary);
    if(secondary && *secondary)
    {
        if(strlen(secondary) > 200 || strchr(secondary, '\n'))
        {
            HWND mle = dw_mle_new(0);
            char **parts = g_strsplit(secondary, "\n", -1);
            char *crlf = g_strjoinv("\r\n", parts);

            dw_mle_set_editable(mle, FALSE);
            dw_mle_set_word_wrap(mle, TRUE);
            dw_mle_import(mle, crlf, 0);
            dw_mle_set_cursor(mle, 0);
            dw_box_pack_start(box, mle, 420, 120, TRUE, TRUE, 3);
            g_free(crlf);
            g_strfreev(parts);
            big = TRUE;
        }
        else
            add_text(box, secondary);
    }
    if(body_html)
    {
        HWND mle = dw_mle_new(0);
        char *text = dwp_html_to_text(body_html, FALSE);
        char **parts = g_strsplit(text, "\n", -1);
        char *crlf = g_strjoinv("\r\n", parts);

        dw_mle_set_editable(mle, FALSE);
        dw_mle_set_word_wrap(mle, TRUE);
        dw_mle_import(mle, crlf, 0);
        dw_mle_set_cursor(mle, 0);
        dw_box_pack_start(box, mle, 460, 240, TRUE, TRUE, 3);
        g_free(crlf);
        g_strfreev(parts);
        g_free(text);
        big = TRUE;
    }
    buttons = dw_box_new(DW_HORZ, 0);
    dw_box_pack_start(box, buttons, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 2);
    dwp_spacer(buttons);
    if(url && *url)
    {
        n->url = g_strdup(url);
        dwp_button(buttons, "Open", DW_SIGNAL_FUNC(note_open_url), n);
    }
    closeb = dwp_button(buttons, "Close", DW_SIGNAL_FUNC(note_close), n);
    dw_signal_connect(n->window, DW_SIGNAL_DELETE, DW_SIGNAL_FUNC(note_close), n);
    dwp_window_present(n->window, big ? 520 : 440, big ? 400 : 240);
    dw_window_set_focus(closeb);
    return n;
}

static void *notify_message(PurpleNotifyMsgType type, const char *title,
                            const char *primary, const char *secondary)
{
    const char *kind = type == PURPLE_NOTIFY_MSG_ERROR ? "Error"
                     : type == PURPLE_NOTIFY_MSG_WARNING ? "Warning" : "Information";

    return simple_window(PURPLE_NOTIFY_MESSAGE, title && *title ? title : kind, primary, secondary, NULL, NULL);
}

static void *notify_email(PurpleConnection *gc, const char *subject, const char *from,
                          const char *to, const char *url)
{
    PurpleAccount *account = purple_connection_get_account(gc);
    char *text = g_strdup_printf("From: %s\nTo: %s\nSubject: %s", from ? from : "",
                                 to && *to ? to : purple_account_get_username(account),
                                 subject ? subject : "");
    Note *n = simple_window(PURPLE_NOTIFY_EMAIL, "New Mail", "You have new mail!", text, NULL, url);

    g_free(text);
    return n;
}

static void *notify_emails(PurpleConnection *gc, size_t count, gboolean detailed,
                           const char **subjects, const char **froms, const char **tos,
                           const char **urls)
{
    PurpleAccount *account = purple_connection_get_account(gc);
    char *name = dwp_account_display_name(account);
    char *primary = g_strdup_printf("%s has %lu new message%s.", name, (unsigned long)count, count == 1 ? "" : "s");
    GString *secondary = g_string_new(NULL);
    Note *n;
    size_t z;

    (void)tos;
    if(detailed && subjects)
        for(z = 0; z < count; z++)
            g_string_append_printf(secondary, "%s - %s\n", froms && froms[z] ? froms[z] : "",
                                   subjects[z] ? subjects[z] : "");
    n = simple_window(PURPLE_NOTIFY_EMAILS, "New Mail", primary,
                      secondary->len ? g_strstrip(secondary->str) : NULL, NULL,
                      urls && urls[0] ? urls[0] : NULL);
    g_string_free(secondary, TRUE);
    g_free(primary);
    g_free(name);
    return n;
}

static void *notify_formatted(const char *title, const char *primary, const char *secondary, const char *text)
{
    return simple_window(PURPLE_NOTIFY_FORMATTED, title, primary, secondary, text ? text : "", NULL);
}

static void *notify_userinfo(PurpleConnection *gc, const char *who, PurpleNotifyUserInfo *user_info)
{
    char *text = purple_notify_user_info_get_text_with_newline(user_info, "<br />");
    char *acct = dwp_account_display_name(purple_connection_get_account(gc));
    char *primary = g_strdup_printf("Info for %s", who ? who : "");
    Note *n = simple_window(PURPLE_NOTIFY_USERINFO, "User Info", primary, acct,
                            text && *text ? text : "(no information)", NULL);

    g_free(primary);
    g_free(acct);
    g_free(text);
    return n;
}

static void *notify_uri(const char *uri)
{
    if(!dwp_open_link(uri))
        purple_notify_error(NULL, "Open Link", "Refusing to open this link.", uri);
    return NULL;
}

/* ========================================================= search results */

static char *button_label(PurpleNotifySearchButton *button)
{
    switch(button->type)
    {
        case PURPLE_NOTIFY_BUTTON_LABELED:  return dwp_strip_mnemonic(button->label);
        case PURPLE_NOTIFY_BUTTON_CONTINUE: return g_strdup("Continue");
        case PURPLE_NOTIFY_BUTTON_ADD:      return g_strdup("Add");
        case PURPLE_NOTIFY_BUTTON_INFO:     return g_strdup("Get Info");
        case PURPLE_NOTIFY_BUTTON_IM:       return g_strdup("Send Message");
        case PURPLE_NOTIFY_BUTTON_JOIN:     return g_strdup("Join");
        case PURPLE_NOTIFY_BUTTON_INVITE:   return g_strdup("Invite");
    }
    return g_strdup("");
}

static void search_set_rows(Note *n, PurpleNotifySearchResults *results)
{
    GList *r;
    int count = 0, row = 0;
    void *alloc;

    dw_container_clear(n->list, TRUE);
    count = (int)g_list_length(results->rows);
    if(!count)
        return;
    alloc = dw_container_alloc(n->list, count);
    for(r = results->rows; r; r = r->next, row++)
    {
        GList *c;
        int col = 0;

        for(c = r->data; c && col < n->columns; c = c->next, col++)
        {
            char *text = c->data ? (char *)c->data : "";

            dw_container_set_item(n->list, alloc, col, row, &text);
        }
        for(; col < n->columns; col++)
        {
            char *empty = "";

            dw_container_set_item(n->list, alloc, col, row, &empty);
        }
        dw_container_set_row_title(alloc, row, ((GList *)r->data && ((GList *)r->data)->data)
                                               ? (char *)((GList *)r->data)->data : "");
        dw_container_set_row_data(alloc, row, GINT_TO_POINTER(row + 1));
    }
    dw_container_insert(n->list, alloc, count);
    dw_container_optimize(n->list);
}

static int DWSIGNAL search_button_clicked(HWND w, void *data)
{
    SearchButton *sb = data;
    Note *n = sb->note;
    PurpleConnection *conn;
    GList *row = NULL;
    int index;

    (void)w;
    if(n->closing || !dwp_account_exists(n->account))
        return TRUE;
    conn = purple_account_get_connection(n->account);
    if(!conn || !sb->button->callback)
        return TRUE;
    index = GPOINTER_TO_INT(dw_container_query_start(n->list, DW_CRA_SELECTED | DW_CR_RETDATA)) - 1;
    if(index >= 0)
    {
        GList *src = g_list_nth_data(n->results->rows, index), *c;

        /* Like Pidgin: pass a freshly allocated copy of the selected row. */
        for(c = src; c; c = c->next)
            row = g_list_append(row, g_strdup(c->data ? c->data : ""));
    }
    sb->button->callback(conn, row, n->user_data);
    g_list_free_full(row, g_free);
    return TRUE;
}

static void *notify_searchresults(PurpleConnection *gc, const char *title, const char *primary,
                                  const char *secondary, PurpleNotifySearchResults *results,
                                  gpointer user_data)
{
    Note *n = note_new(PURPLE_NOTIFY_SEARCHRESULTS, title);
    HWND box = dw_box_new(DW_VERT, 4), buttons;
    GList *c, *b;
    unsigned long *flags;
    char **titles;
    int z;

    n->account = purple_connection_get_account(gc);
    n->results = results;
    n->user_data = user_data;
    n->buttons = g_ptr_array_new_with_free_func(g_free);
    n->columns = (int)g_list_length(results->columns);
    dw_box_pack_start(n->window, box, 0, 0, TRUE, TRUE, 0);
    if(primary && *primary)
        add_text(box, primary);
    if(secondary && *secondary)
        add_text(box, secondary);

    n->list = dw_container_new(0, FALSE);
    if(n->columns > 0)
    {
        flags = g_new0(unsigned long, n->columns);
        titles = g_new0(char *, n->columns);
        for(c = results->columns, z = 0; c; c = c->next, z++)
        {
            flags[z] = DW_CFA_STRING | DW_CFA_LEFT | DW_CFA_HORZSEPARATOR | DW_CFA_SEPARATOR;
            titles[z] = ((PurpleNotifySearchColumn *)c->data)->title;
        }
        dw_container_setup(n->list, flags, titles, n->columns, 0);
        g_free(flags);
        g_free(titles);
    }
    dw_box_pack_start(box, n->list, 560, 300, TRUE, TRUE, 2);

    buttons = dw_box_new(DW_HORZ, 0);
    dw_box_pack_start(box, buttons, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 2);
    for(b = results->buttons; b; b = b->next)
    {
        SearchButton *sb = g_new0(SearchButton, 1);
        char *label;

        sb->note = n;
        sb->button = b->data;
        g_ptr_array_add(n->buttons, sb);
        label = button_label(sb->button);
        dwp_button(buttons, label, DW_SIGNAL_FUNC(search_button_clicked), sb);
        g_free(label);
    }
    dwp_spacer(buttons);
    dwp_button(buttons, "Close", DW_SIGNAL_FUNC(note_close), n);
    dw_signal_connect(n->window, DW_SIGNAL_DELETE, DW_SIGNAL_FUNC(note_close), n);
    if(n->columns > 0)
        search_set_rows(n, results);
    dwp_window_present(n->window, 640, 440);
    return n;
}

static void notify_searchresults_new_rows(PurpleConnection *gc, PurpleNotifySearchResults *results, void *data)
{
    Note *n = data;

    (void)gc;
    if(!g_list_find(live, n) || n->closing || n->columns <= 0)
        return;
    search_set_rows(n, results);
}

static void close_notify(PurpleNotifyType type, void *ui_handle)
{
    Note *n = ui_handle;

    (void)type;
    if(g_list_find(live, n))
        close_from_core(n);
}

PurpleNotifyUiOps *dwp_notify_ui_ops(void)
{
    static PurpleNotifyUiOps ops;
    static gboolean ready = FALSE;

    if(!ready)
    {
        memset(&ops, 0, sizeof(ops));
        ops.notify_message = notify_message;
        ops.notify_email = notify_email;
        ops.notify_emails = notify_emails;
        ops.notify_formatted = notify_formatted;
        ops.notify_searchresults = notify_searchresults;
        ops.notify_searchresults_new_rows = notify_searchresults_new_rows;
        ops.notify_userinfo = notify_userinfo;
        ops.notify_uri = notify_uri;
        ops.close_notify = close_notify;
        ready = TRUE;
    }
    return &ops;
}
