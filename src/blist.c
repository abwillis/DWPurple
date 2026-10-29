/*
 * DWPurple - buddy list window, blist UI ops and buddy/chat/group dialogs.
 *
 * The list is a Dynamic Windows container (not dw_tree): containers deliver
 * double-click (DW_SIGNAL_ITEM_ENTER) on every platform, trees do not on
 * GTK.  Groups are header rows; double-clicking one collapses/expands it.
 * Each row's data is the PurpleBlistNode it shows.  The container is
 * cleared synchronously whenever a shown node is removed, so a row can
 * never refer to freed memory.
 */
#include "dwpurple.h"

typedef struct {
    HWND window, menubar_box, list, status_combo, status_msg, status_bar;
    HWND tray_anchor;
    HMENUI menubar, buddies_menu, tools_menu;
    gboolean rebuild_pending, dead, syncing_status;
    GHashTable *shown;            /* nodes currently in the container */
    GHashTable *expanded;         /* contacts showing their buddies */
    guint status_clear;
    gboolean visible;
} BList;

static BList bl;
static int blist_handle;

static const PurpleStatusPrimitive status_prims[] = {
    PURPLE_STATUS_AVAILABLE, PURPLE_STATUS_AWAY, PURPLE_STATUS_UNAVAILABLE,
    PURPLE_STATUS_EXTENDED_AWAY, PURPLE_STATUS_INVISIBLE, PURPLE_STATUS_OFFLINE
};
#define N_STATUS ((int)(sizeof(status_prims) / sizeof(status_prims[0])))
static char *status_labels[N_STATUS];

static void schedule_rebuild(void);

/* ============================================================== helpers */

void dwp_open_im(PurpleAccount *account, const char *name)
{
    PurpleConversation *conv;

    if(!dwp_account_exists(account) || !name || !*name)
        return;
    conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM, name, account);
    if(!conv)
        conv = purple_conversation_new(PURPLE_CONV_TYPE_IM, account, name);
    if(conv)
        purple_conversation_present(conv);
}

static void join_chat_node(PurpleChat *chat)
{
    PurpleAccount *account = purple_chat_get_account(chat);
    PurpleConnection *gc = purple_account_get_connection(account);

    if(!gc)
    {
        purple_notify_error(NULL, "Join Chat", "The account is not connected.", NULL);
        return;
    }
    serv_join_chat(gc, purple_chat_get_components(chat));
}

static int count_shown_buddies(PurpleContact *contact, gboolean show_offline, PurpleBuddy **first)
{
    PurpleBlistNode *cnode = (PurpleBlistNode *)contact, *n;
    gboolean contact_show = purple_blist_node_get_bool(cnode, "show_offline");
    int count = 0;

    if(first)
        *first = NULL;
    for(n = purple_blist_node_get_first_child(cnode); n; n = purple_blist_node_get_sibling_next(n))
    {
        PurpleBuddy *b;

        if(!PURPLE_BLIST_NODE_IS_BUDDY(n) || !PURPLE_BLIST_NODE_IS_VISIBLE(n))
            continue;
        b = (PurpleBuddy *)n;
        if(!purple_account_is_connected(purple_buddy_get_account(b)))
            continue;
        if(!PURPLE_BUDDY_IS_ONLINE(b) && !show_offline && !contact_show &&
           !purple_blist_node_get_bool(n, "show_offline"))
            continue;
        if(first && !*first)
            *first = b;
        count++;
    }
    return count;
}

static gboolean buddy_is_shown(PurpleBuddy *b, gboolean show_offline)
{
    PurpleBlistNode *n = (PurpleBlistNode *)b;
    PurpleBlistNode *cnode = purple_blist_node_get_parent(n);

    if(!PURPLE_BLIST_NODE_IS_VISIBLE(n) || !purple_account_is_connected(purple_buddy_get_account(b)))
        return FALSE;
    return PURPLE_BUDDY_IS_ONLINE(b) || show_offline ||
           purple_blist_node_get_bool(n, "show_offline") ||
           (cnode && purple_blist_node_get_bool(cnode, "show_offline"));
}

static char *buddy_status_text(PurpleBuddy *buddy)
{
    PurplePluginProtocolInfo *info = dwp_account_prpl_info(purple_buddy_get_account(buddy), NULL);
    PurplePresence *presence = purple_buddy_get_presence(buddy);
    PurpleStatusPrimitive prim;
    gboolean idle;
    GString *out = g_string_new(NULL);

    prim = dwp_buddy_primitive(buddy, &idle);
    g_string_append(out, dwp_primitive_label(prim));
    if(info && info->status_text && PURPLE_BUDDY_IS_ONLINE(buddy))
    {
        char *t = info->status_text(buddy);

        if(t && *t)
        {
            char *plain = purple_markup_strip_html(t);
            char *p;

            for(p = plain; *p; p++)
                if(*p == '\n' || *p == '\r' || *p == '\t')
                    *p = ' ';
            g_string_append_printf(out, " - %s", g_strstrip(plain));
            g_free(plain);
        }
        g_free(t);
    }
    if(idle)
    {
        time_t since = purple_presence_get_idle_time(presence);

        if(since > 0)
        {
            long mins = (long)((time(NULL) - since) / 60);

            if(mins > 0)
                g_string_append_printf(out, " (idle %ldm)", mins);
        }
        else
            g_string_append(out, " (idle)");
    }
    return g_string_free(out, FALSE);
}

/* =============================================================== rebuild */

typedef struct {
    char *name, *status;
    HICN icon;
    PurpleBlistNode *node;
} Row;

static void add_row(GPtrArray *rows, const char *name, const char *status, HICN icon, PurpleBlistNode *node)
{
    Row *r = g_new0(Row, 1);

    r->name = g_strdup(name);
    r->status = g_strdup(status ? status : "");
    r->icon = icon;
    r->node = node;
    g_ptr_array_add(rows, r);
}

static void row_free(gpointer p)
{
    Row *r = p;

    g_free(r->name);
    g_free(r->status);
    g_free(r);
}

static void clear_list(void)
{
    if(bl.list)
        dw_container_clear(bl.list, TRUE);
    if(bl.shown)
        g_hash_table_remove_all(bl.shown);
}

static PurpleBlistNode *selected_node(void)
{
    char *data;

    if(!bl.list)
        return NULL;
    data = dw_container_query_start(bl.list, DW_CRA_SELECTED | DW_CR_RETDATA);
    return (PurpleBlistNode *)data;
}

static void rebuild(void)
{
    gboolean show_offline, show_empty;
    PurpleBlistNode *gnode, *selected;
    GPtrArray *rows;
    void *alloc;
    guint z;

    bl.rebuild_pending = FALSE;
    if(bl.dead || !bl.list || !purple_get_blist())
        return;

    show_offline = purple_prefs_get_bool(DWP_PREF_OFFLINE);
    show_empty = purple_prefs_get_bool(DWP_PREF_EMPTY);
    selected = selected_node();
    if(selected && !g_hash_table_lookup(bl.shown, selected))
        selected = NULL;

    rows = g_ptr_array_new_with_free_func(row_free);
    for(gnode = purple_blist_get_root(); gnode; gnode = purple_blist_node_get_sibling_next(gnode))
    {
        PurpleGroup *group;
        GPtrArray *children;
        PurpleBlistNode *n;
        gboolean collapsed;
        char *title;

        if(!PURPLE_BLIST_NODE_IS_GROUP(gnode) || !PURPLE_BLIST_NODE_IS_VISIBLE(gnode))
            continue;
        group = (PurpleGroup *)gnode;
        collapsed = purple_blist_node_get_bool(gnode, "collapsed");
        children = g_ptr_array_new_with_free_func(row_free);

        for(n = purple_blist_node_get_first_child(gnode); n; n = purple_blist_node_get_sibling_next(n))
        {
            if(PURPLE_BLIST_NODE_IS_CONTACT(n) && PURPLE_BLIST_NODE_IS_VISIBLE(n))
            {
                PurpleContact *contact = (PurpleContact *)n;
                PurpleBuddy *first, *priority;
                int count = count_shown_buddies(contact, show_offline, &first);
                gboolean idle;
                PurpleStatusPrimitive prim;
                char *name, *status;

                if(!count)
                    continue;
                priority = purple_contact_get_priority_buddy(contact);
                if(!priority || !buddy_is_shown(priority, show_offline))
                    priority = first;
                prim = dwp_buddy_primitive(priority, &idle);
                status = buddy_status_text(priority);
                if(count > 1)
                    name = g_strdup_printf("    %s %s", g_hash_table_lookup(bl.expanded, n) ? "-" : "+",
                                           purple_contact_get_alias(contact));
                else
                    name = g_strdup_printf("    %s", purple_contact_get_alias(contact));
                add_row(children, name, status, dwp_status_icon(prim, idle), n);
                g_free(name);
                g_free(status);

                if(count > 1 && g_hash_table_lookup(bl.expanded, n))
                {
                    PurpleBlistNode *b;

                    for(b = purple_blist_node_get_first_child(n); b; b = purple_blist_node_get_sibling_next(b))
                    {
                        PurpleBuddy *buddy;

                        if(!PURPLE_BLIST_NODE_IS_BUDDY(b))
                            continue;
                        buddy = (PurpleBuddy *)b;
                        if(!buddy_is_shown(buddy, show_offline))
                            continue;
                        prim = dwp_buddy_primitive(buddy, &idle);
                        name = g_strdup_printf("          %s [%s]", purple_buddy_get_alias(buddy),
                                               purple_account_get_username(purple_buddy_get_account(buddy)));
                        status = buddy_status_text(buddy);
                        add_row(children, name, status, dwp_status_icon(prim, idle), b);
                        g_free(name);
                        g_free(status);
                    }
                }
            }
            else if(PURPLE_BLIST_NODE_IS_CHAT(n) && PURPLE_BLIST_NODE_IS_VISIBLE(n))
            {
                PurpleChat *chat = (PurpleChat *)n;
                char *name;

                if(!purple_account_is_connected(purple_chat_get_account(chat)))
                    continue;
                name = g_strdup_printf("    %s", purple_chat_get_name(chat));
                add_row(children, name, "Chat", dwp_chat_icon(), n);
                g_free(name);
            }
        }
        if(!children->len && !show_empty)
        {
            g_ptr_array_free(children, TRUE);
            continue;
        }
        title = g_strdup_printf("[%s] %s (%d/%d)", collapsed ? "+" : "-",
                                purple_group_get_name(group),
                                purple_blist_get_group_online_count(group),
                                purple_blist_get_group_size(group, FALSE));
        add_row(rows, title, "", 0, gnode);
        g_free(title);
        if(!collapsed)
        {
            for(z = 0; z < children->len; z++)
            {
                Row *r = g_ptr_array_index(children, z);

                add_row(rows, r->name, r->status, r->icon, r->node);
            }
        }
        g_ptr_array_free(children, TRUE);
    }

    clear_list();
    if(rows->len)
    {
        alloc = dw_container_alloc(bl.list, (int)rows->len);
        for(z = 0; z < rows->len; z++)
        {
            Row *r = g_ptr_array_index(rows, z);
            char *status = r->status;

            dw_filesystem_set_file(bl.list, alloc, (int)z, r->name, r->icon);
            dw_filesystem_set_item(bl.list, alloc, 0, (int)z, &status);
            dw_container_set_row_title(alloc, (int)z, r->name);
            dw_container_set_row_data(alloc, (int)z, r->node);
            g_hash_table_insert(bl.shown, r->node, GINT_TO_POINTER(1));
        }
        dw_container_insert(bl.list, alloc, (int)rows->len);
        if(selected && g_hash_table_lookup(bl.shown, selected))
            dw_container_cursor_by_data(bl.list, selected);
    }
    g_ptr_array_free(rows, TRUE);
    dwp_tray_update();
}

static void rebuild_task(gpointer unused)
{
    (void)unused;
    if(bl.rebuild_pending)
        rebuild();
}

static void schedule_rebuild(void)
{
    if(bl.dead || bl.rebuild_pending)
        return;
    bl.rebuild_pending = TRUE;
    dwp_defer(rebuild_task, NULL);
}

/* ====================================================== status selection */

static void sync_status_box(void)
{
    PurpleSavedStatus *saved = purple_savedstatus_get_current();
    PurpleStatusPrimitive prim;
    const char *msg;
    int z;

    if(!saved || !bl.status_combo)
        return;
    bl.syncing_status = TRUE;
    prim = purple_savedstatus_get_type(saved);
    for(z = 0; z < N_STATUS; z++)
    {
        if(status_prims[z] == prim)
        {
            dw_listbox_select(bl.status_combo, z, TRUE);
            dw_window_set_text(bl.status_combo, status_labels[z]);
        }
    }
    msg = purple_savedstatus_get_message(saved);
    if(msg && *msg)
    {
        char *plain = purple_markup_strip_html(msg);

        dw_window_set_text(bl.status_msg, plain);
        g_free(plain);
    }
    else
        dw_window_set_text(bl.status_msg, "");
    bl.syncing_status = FALSE;
}

static void apply_status(void)
{
    int index = dwp_combo_index(bl.status_combo, status_labels, N_STATUS);
    PurpleStatusPrimitive prim = index >= 0 ? status_prims[index] : PURPLE_STATUS_AVAILABLE;
    char *text = dwp_get_text_trimmed(bl.status_msg);
    char *html = *text ? g_markup_escape_text(text, -1) : NULL;
    PurpleSavedStatus *saved;

    saved = purple_savedstatus_find_transient_by_type_and_message(prim, html);
    if(!saved)
    {
        saved = purple_savedstatus_new(NULL, prim);
        purple_savedstatus_set_message(saved, html);
    }
    purple_savedstatus_activate(saved);
    g_free(html);
    g_free(text);
}

void dwp_blist_set_status_primitive(PurpleStatusPrimitive prim)
{
    int z;

    for(z = 0; z < N_STATUS; z++)
    {
        if(status_prims[z] == prim)
        {
            bl.syncing_status = TRUE;
            dw_listbox_select(bl.status_combo, z, TRUE);
            dw_window_set_text(bl.status_combo, status_labels[z]);
            bl.syncing_status = FALSE;
            apply_status();
            return;
        }
    }
}

static int DWSIGNAL status_selected(HWND w, int index, void *data)
{
    (void)w;
    (void)index;
    (void)data;
    if(!bl.syncing_status)
        apply_status();
    return FALSE;
}

static int DWSIGNAL status_msg_set(HWND w, void *data)
{
    (void)w;
    (void)data;
    if(!bl.syncing_status)
        apply_status();
    return FALSE;
}

static void on_savedstatus_changed(PurpleSavedStatus *now, PurpleSavedStatus *old, gpointer data)
{
    (void)now;
    (void)old;
    (void)data;
    sync_status_box();
    dwp_tray_update();
}

static gboolean status_clear_cb(gpointer data)
{
    (void)data;
    bl.status_clear = 0;
    if(bl.status_bar)
        dw_window_set_text(bl.status_bar, "");
    return FALSE;
}

void dwp_blist_status_message(const char *text)
{
    if(!bl.status_bar)
        return;
    dw_window_set_text(bl.status_bar, text ? text : "");
    if(bl.status_clear)
        purple_timeout_remove(bl.status_clear);
    bl.status_clear = purple_timeout_add_seconds(10, status_clear_cb, NULL);
}

/* ============================================================ menus/node */

typedef struct {
    PurpleCallback cb;
    gpointer data;
} ProtoAct;

static GPtrArray *proto_acts = NULL;

static void proto_action(gpointer node, gpointer act, const char *unused)
{
    ProtoAct *pa = act;
    PurpleCallback cb = pa->cb;
    gpointer data = pa->data;

    (void)unused;
    if(!dwp_blist_node_exists(node))
        return;
    ((void (*)(gpointer, gpointer))cb)(node, data);
}

static void add_menu_actions(HMENUI menu, GList *list, PurpleBlistNode *node)
{
    GList *l;

    for(l = list; l; l = l->next)
    {
        PurpleMenuAction *act = l->data;
        char *label;

        if(!act)
        {
            dwp_menu_separator(menu);
            continue;
        }
        label = dwp_mnemonic(act->label);
        if(act->children)
        {
            HMENUI sub = dw_menu_new(0);

            add_menu_actions(sub, act->children, node);
            dw_menu_append_item(menu, label, DW_MENU_POPUP, 0, TRUE, FALSE, sub);
        }
        else if(act->callback)
        {
            ProtoAct *pa = g_new0(ProtoAct, 1);

            pa->cb = act->callback;
            pa->data = act->data;
            g_ptr_array_add(proto_acts, pa);
            dwp_menu_add(menu, label, proto_action, node, pa, NULL);
        }
        else
            dw_menu_append_item(menu, label, DW_MENU_POPUP, 0, TRUE, FALSE, DW_NOMENU);
        g_free(label);
    }
}

static void free_menu_actions(GList *list)
{
    GList *l;

    for(l = list; l; l = l->next)
    {
        PurpleMenuAction *act = l->data;

        if(!act)
            continue;
        free_menu_actions(act->children);
        act->children = NULL;
        purple_menu_action_free(act);
    }
    g_list_free(list);
}

static void reset_popup_state(void)
{
    dwp_actions_reset();
    if(!proto_acts)
        proto_acts = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_set_size(proto_acts, 0);
}

static void add_protocol_menus(HMENUI menu, PurpleBlistNode *node, PurpleAccount *account)
{
    PurplePluginProtocolInfo *info = dwp_account_prpl_info(account, NULL);
    GList *ext;

    if(info && info->blist_node_menu)
    {
        GList *actions = info->blist_node_menu(node);

        if(actions)
        {
            dwp_menu_separator(menu);
            add_menu_actions(menu, actions, node);
            free_menu_actions(actions);
        }
    }
    ext = purple_blist_node_get_extended_menu(node);
    if(ext)
    {
        dwp_menu_separator(menu);
        add_menu_actions(menu, ext, node);
        free_menu_actions(ext);
    }
}

static void act_send_im(gpointer node, gpointer b, const char *s)
{
    PurpleBuddy *buddy;

    (void)b;
    (void)s;
    if(!dwp_blist_node_exists(node))
        return;
    if(PURPLE_BLIST_NODE_IS_CONTACT((PurpleBlistNode *)node))
        buddy = purple_contact_get_priority_buddy((PurpleContact *)node);
    else
        buddy = (PurpleBuddy *)node;
    if(buddy)
        dwp_open_im(purple_buddy_get_account(buddy), purple_buddy_get_name(buddy));
}

static PurpleBuddy *node_buddy(gpointer node)
{
    if(!dwp_blist_node_exists(node))
        return NULL;
    if(PURPLE_BLIST_NODE_IS_CONTACT((PurpleBlistNode *)node))
        return purple_contact_get_priority_buddy((PurpleContact *)node);
    if(PURPLE_BLIST_NODE_IS_BUDDY((PurpleBlistNode *)node))
        return (PurpleBuddy *)node;
    return NULL;
}

static void act_send_file(gpointer node, gpointer b, const char *s)
{
    PurpleBuddy *buddy = node_buddy(node);
    PurpleConnection *gc;

    (void)b;
    (void)s;
    if(!buddy || !(gc = purple_account_get_connection(purple_buddy_get_account(buddy))))
        return;
    serv_send_file(gc, purple_buddy_get_name(buddy), NULL);
}

static void act_get_info(gpointer node, gpointer b, const char *s)
{
    PurpleBuddy *buddy = node_buddy(node);
    PurpleConnection *gc;

    (void)b;
    (void)s;
    if(!buddy || !(gc = purple_account_get_connection(purple_buddy_get_account(buddy))))
        return;
    serv_get_info(gc, purple_buddy_get_name(buddy));
}

static void act_pounce(gpointer node, gpointer b, const char *s)
{
    PurpleBuddy *buddy = node_buddy(node);

    (void)b;
    (void)s;
    if(buddy)
        dwp_pounce_edit(NULL, purple_buddy_get_account(buddy), purple_buddy_get_name(buddy));
}

static void act_block(gpointer node, gpointer block, const char *s)
{
    PurpleBuddy *buddy = node_buddy(node);

    (void)s;
    if(buddy)
        dwp_privacy_set_blocked(purple_buddy_get_account(buddy), purple_buddy_get_name(buddy),
                                GPOINTER_TO_INT(block));
}

static void alias_done(gpointer node, const char *text)
{
    char *alias;

    if(!dwp_blist_node_exists(node))
        return;
    alias = g_strstrip(g_strdup(text ? text : ""));
    if(PURPLE_BLIST_NODE_IS_CONTACT((PurpleBlistNode *)node))
        purple_blist_alias_contact((PurpleContact *)node, *alias ? alias : NULL);
    else if(PURPLE_BLIST_NODE_IS_BUDDY((PurpleBlistNode *)node))
    {
        PurpleBuddy *buddy = node;

        purple_blist_alias_buddy(buddy, *alias ? alias : NULL);
        serv_alias_buddy(buddy);
    }
    else if(PURPLE_BLIST_NODE_IS_CHAT((PurpleBlistNode *)node))
        purple_blist_alias_chat((PurpleChat *)node, *alias ? alias : NULL);
    g_free(alias);
}

static void act_alias(gpointer node, gpointer b, const char *s)
{
    const char *current = "";

    (void)b;
    (void)s;
    if(!dwp_blist_node_exists(node))
        return;
    if(PURPLE_BLIST_NODE_IS_CONTACT((PurpleBlistNode *)node))
        current = purple_contact_get_alias((PurpleContact *)node);
    else if(PURPLE_BLIST_NODE_IS_BUDDY((PurpleBlistNode *)node))
        current = purple_buddy_get_local_buddy_alias((PurpleBuddy *)node);
    else if(PURPLE_BLIST_NODE_IS_CHAT((PurpleBlistNode *)node))
        current = purple_chat_get_name((PurpleChat *)node);
    dwp_prompt("Alias", "Enter an alias:", current, FALSE, alias_done, node);
}

static void act_show_offline(gpointer node, gpointer b, const char *s)
{
    (void)b;
    (void)s;
    if(!dwp_blist_node_exists(node))
        return;
    purple_blist_node_set_bool(node, "show_offline", !purple_blist_node_get_bool(node, "show_offline"));
    schedule_rebuild();
}

static void act_expand(gpointer node, gpointer b, const char *s)
{
    (void)b;
    (void)s;
    if(!dwp_blist_node_exists(node))
        return;
    if(g_hash_table_lookup(bl.expanded, node))
        g_hash_table_remove(bl.expanded, node);
    else
        g_hash_table_insert(bl.expanded, node, GINT_TO_POINTER(1));
    schedule_rebuild();
}

static void act_move(gpointer node, gpointer group, const char *s)
{
    (void)s;
    if(!dwp_blist_node_exists(node) || !dwp_blist_node_exists(group))
        return;
    if(PURPLE_BLIST_NODE_IS_CONTACT((PurpleBlistNode *)node))
        purple_blist_add_contact((PurpleContact *)node, (PurpleGroup *)group, NULL);
    else if(PURPLE_BLIST_NODE_IS_BUDDY((PurpleBlistNode *)node))
        purple_blist_add_buddy((PurpleBuddy *)node, NULL, (PurpleGroup *)group, NULL);
    else if(PURPLE_BLIST_NODE_IS_CHAT((PurpleBlistNode *)node))
        purple_blist_add_chat((PurpleChat *)node, (PurpleGroup *)group, NULL);
}

static void remove_buddy_from_server(PurpleBuddy *buddy)
{
    PurpleAccount *account = purple_buddy_get_account(buddy);

    if(purple_account_is_connected(account))
        purple_account_remove_buddy(account, buddy, purple_buddy_get_group(buddy));
}

static void act_remove(gpointer node, gpointer b, const char *s)
{
    PurpleBlistNode *n = node;

    (void)b;
    (void)s;
    if(!dwp_blist_node_exists(n))
        return;
    if(PURPLE_BLIST_NODE_IS_CONTACT(n))
    {
        char *name = g_strdup(purple_contact_get_alias((PurpleContact *)n));
        gboolean ok = dwp_confirm("Remove Contact", "Remove %s from your buddy list?", name);
        PurpleBlistNode *c;

        g_free(name);
        if(!ok || !dwp_blist_node_exists(n))
            return;
        for(c = purple_blist_node_get_first_child(n); c; c = purple_blist_node_get_sibling_next(c))
            if(PURPLE_BLIST_NODE_IS_BUDDY(c))
                remove_buddy_from_server((PurpleBuddy *)c);
        purple_blist_remove_contact((PurpleContact *)n);
    }
    else if(PURPLE_BLIST_NODE_IS_BUDDY(n))
    {
        char *name = g_strdup(purple_buddy_get_alias((PurpleBuddy *)n));
        gboolean ok = dwp_confirm("Remove Buddy", "Remove %s from your buddy list?", name);

        g_free(name);
        if(!ok || !dwp_blist_node_exists(n))
            return;
        remove_buddy_from_server((PurpleBuddy *)n);
        purple_blist_remove_buddy((PurpleBuddy *)n);
    }
    else if(PURPLE_BLIST_NODE_IS_CHAT(n))
    {
        char *name = g_strdup(purple_chat_get_name((PurpleChat *)n));
        gboolean ok = dwp_confirm("Remove Chat", "Remove the chat %s from your buddy list?", name);

        g_free(name);
        if(ok && dwp_blist_node_exists(n))
            purple_blist_remove_chat((PurpleChat *)n);
    }
    else if(PURPLE_BLIST_NODE_IS_GROUP(n))
    {
        char *name = g_strdup(purple_group_get_name((PurpleGroup *)n));
        gboolean ok = dwp_confirm("Remove Group", "Remove the group %s and all its buddies and chats?", name);
        PurpleBlistNode *c, *next;
        GList *l;

        g_free(name);
        if(!ok || !dwp_blist_node_exists(n))
            return;
        for(c = purple_blist_node_get_first_child(n); c; c = next)
        {
            next = purple_blist_node_get_sibling_next(c);
            if(PURPLE_BLIST_NODE_IS_CONTACT(c))
            {
                PurpleBlistNode *bn;

                for(bn = purple_blist_node_get_first_child(c); bn; bn = purple_blist_node_get_sibling_next(bn))
                    if(PURPLE_BLIST_NODE_IS_BUDDY(bn))
                        remove_buddy_from_server((PurpleBuddy *)bn);
                purple_blist_remove_contact((PurpleContact *)c);
            }
            else if(PURPLE_BLIST_NODE_IS_CHAT(c))
                purple_blist_remove_chat((PurpleChat *)c);
        }
        for(l = purple_connections_get_all(); l; l = l->next)
        {
            PurpleAccount *account = purple_connection_get_account(l->data);

            if(purple_account_is_connected(account))
                purple_account_remove_group(account, (PurpleGroup *)n);
        }
        purple_blist_remove_group((PurpleGroup *)n);
    }
}

static void act_join(gpointer node, gpointer b, const char *s)
{
    (void)b;
    (void)s;
    if(dwp_blist_node_exists(node))
        join_chat_node(node);
}

static void act_add_buddy_to(gpointer node, gpointer b, const char *s)
{
    (void)b;
    (void)s;
    dwp_show_add_buddy(NULL, NULL, dwp_blist_node_exists(node) ? purple_group_get_name(node) : NULL, NULL);
}

static void act_add_chat_to(gpointer node, gpointer b, const char *s)
{
    (void)b;
    (void)s;
    dwp_show_add_chat(NULL, dwp_blist_node_exists(node) ? purple_group_get_name(node) : NULL, NULL, NULL);
}

static void rename_group_done(gpointer node, const char *text)
{
    char *name;

    if(!dwp_blist_node_exists(node))
        return;
    name = g_strstrip(g_strdup(text ? text : ""));
    if(*name)
        purple_blist_rename_group(node, name);
    g_free(name);
}

static void act_rename_group(gpointer node, gpointer b, const char *s)
{
    (void)b;
    (void)s;
    if(dwp_blist_node_exists(node))
        dwp_prompt("Rename Group", "New group name:", purple_group_get_name(node), FALSE,
                   rename_group_done, node);
}

static void add_move_menu(HMENUI menu, PurpleBlistNode *node)
{
    HMENUI move = dw_menu_new(0);
    PurpleBlistNode *g;
    PurpleBlistNode *current = PURPLE_BLIST_NODE_IS_BUDDY(node)
        ? purple_blist_node_get_parent(purple_blist_node_get_parent(node))
        : purple_blist_node_get_parent(node);

    for(g = purple_blist_get_root(); g; g = purple_blist_node_get_sibling_next(g))
    {
        if(!PURPLE_BLIST_NODE_IS_GROUP(g) || g == current)
            continue;
        dwp_menu_add(move, purple_group_get_name((PurpleGroup *)g), act_move, node, g, NULL);
    }
    dw_menu_append_item(menu, "~Move to", DW_MENU_POPUP, 0, TRUE, FALSE, move);
}

static void buddy_menu(HMENUI menu, PurpleBlistNode *node)
{
    PurpleBuddy *buddy = node_buddy(node);
    PurpleAccount *account;
    PurpleConnection *gc;
    PurplePluginProtocolInfo *info;
    gboolean blocked;
    int count;

    if(!buddy)
        return;
    account = purple_buddy_get_account(buddy);
    info = dwp_account_prpl_info(account, &gc);
    dwp_menu_add(menu, "Send ~IM", act_send_im, node, NULL, NULL);
    if(info && info->send_file &&
       (!info->can_receive_file || info->can_receive_file(gc, purple_buddy_get_name(buddy))))
        dwp_menu_add(menu, "Send ~File...", act_send_file, node, NULL, NULL);
    if(info && info->get_info)
        dwp_menu_add(menu, "Get ~Info", act_get_info, node, NULL, NULL);
    dwp_menu_add(menu, "Add Buddy ~Pounce...", act_pounce, node, NULL, NULL);
    blocked = dwp_privacy_is_blocked(account, purple_buddy_get_name(buddy));
    dwp_menu_add(menu, blocked ? "Un~block" : "~Block", act_block, node, GINT_TO_POINTER(!blocked), NULL);
    dwp_menu_separator(menu);
    dwp_menu_add(menu, "~Alias...", act_alias, node, NULL, NULL);
    if(PURPLE_BLIST_NODE_IS_CONTACT(node))
    {
        count = count_shown_buddies((PurpleContact *)node, TRUE, NULL);
        if(count > 1)
            dwp_menu_add(menu, g_hash_table_lookup(bl.expanded, node) ? "~Collapse" : "~Expand",
                         act_expand, node, NULL, NULL);
    }
    dwp_menu_add(menu, purple_blist_node_get_bool(node, "show_offline")
                       ? "Don't Show When ~Offline" : "Show When ~Offline",
                 act_show_offline, node, NULL, NULL);
    add_move_menu(menu, node);
    dwp_menu_add(menu, "~Remove", act_remove, node, NULL, NULL);
    add_protocol_menus(menu, (PurpleBlistNode *)buddy, account);
}

static void group_menu(HMENUI menu, PurpleBlistNode *node)
{
    GList *ext;

    dwp_menu_add(menu, "Add ~Buddy...", act_add_buddy_to, node, NULL, NULL);
    dwp_menu_add(menu, "Add C~hat...", act_add_chat_to, node, NULL, NULL);
    dwp_menu_add(menu, "Re~name...", act_rename_group, node, NULL, NULL);
    dwp_menu_add(menu, "~Remove...", act_remove, node, NULL, NULL);
    ext = purple_blist_node_get_extended_menu(node);
    if(ext)
    {
        dwp_menu_separator(menu);
        add_menu_actions(menu, ext, node);
        free_menu_actions(ext);
    }
}

static void act_autojoin(gpointer node, gpointer b, const char *s)
{
    (void)b;
    (void)s;
    if(dwp_blist_node_exists(node))
        purple_blist_node_set_bool(node, "gtk-autojoin", !purple_blist_node_get_bool(node, "gtk-autojoin"));
}

static void chat_menu(HMENUI menu, PurpleBlistNode *node)
{
    dwp_menu_add(menu, "~Join", act_join, node, NULL, NULL);
    dwp_menu_add(menu, "~Alias...", act_alias, node, NULL, NULL);
    dwp_menu_add(menu, purple_blist_node_get_bool(node, "gtk-autojoin") ? "Don't Auto-~Join" : "Auto-~Join",
                 act_autojoin, node, NULL, NULL);
    add_move_menu(menu, node);
    dwp_menu_add(menu, "~Remove", act_remove, node, NULL, NULL);
    add_protocol_menus(menu, node, purple_chat_get_account((PurpleChat *)node));
}

static void generic_add_menu(HMENUI menu)
{
    dwp_menu_add(menu, "Add ~Buddy...", act_add_buddy_to, NULL, NULL, NULL);
    dwp_menu_add(menu, "Add C~hat...", act_add_chat_to, NULL, NULL, NULL);
}

static int DWSIGNAL list_context(HWND w, char *text, int x, int y, void *data, void *itemdata)
{
    PurpleBlistNode *node = itemdata;
    HMENUI menu;

    (void)w;
    (void)text;
    (void)data;
    dwp_note_activity();
    reset_popup_state();
    menu = dw_menu_new(0);
    if(node && g_hash_table_lookup(bl.shown, node) && dwp_blist_node_exists(node))
    {
        if(PURPLE_BLIST_NODE_IS_GROUP(node))
            group_menu(menu, node);
        else if(PURPLE_BLIST_NODE_IS_CHAT(node))
            chat_menu(menu, node);
        else
            buddy_menu(menu, node);
    }
    else
        generic_add_menu(menu);
    dw_menu_popup(&menu, bl.window, x, y);
    return TRUE;
}

static int DWSIGNAL list_enter(HWND w, char *text, void *data, void *itemdata)
{
    PurpleBlistNode *node = itemdata;

    (void)w;
    (void)text;
    (void)data;
    dwp_note_activity();
    if(!node || !g_hash_table_lookup(bl.shown, node) || !dwp_blist_node_exists(node))
        return FALSE;
    if(PURPLE_BLIST_NODE_IS_GROUP(node))
    {
        purple_blist_node_set_bool(node, "collapsed", !purple_blist_node_get_bool(node, "collapsed"));
        schedule_rebuild();
    }
    else if(PURPLE_BLIST_NODE_IS_CHAT(node))
        join_chat_node((PurpleChat *)node);
    else
        act_send_im(node, NULL, NULL);
    return TRUE;
}

/* =============================================================== menubar */

static void menu_new_im(gpointer a, gpointer b, const char *s) { (void)a; (void)b; (void)s; dwp_show_new_im(); }
static void menu_join_chat(gpointer a, gpointer b, const char *s) { (void)a; (void)b; (void)s; dwp_show_join_chat(); }
static void menu_get_info(gpointer a, gpointer b, const char *s) { (void)a; (void)b; (void)s; dwp_show_get_info(); }
static void menu_roomlist(gpointer a, gpointer b, const char *s) { (void)a; (void)b; (void)s; dwp_roomlist_show(NULL, FALSE); }
static void menu_add_buddy(gpointer a, gpointer b, const char *s) { (void)a; (void)b; (void)s; dwp_show_add_buddy(NULL, NULL, NULL, NULL); }
static void menu_add_chat(gpointer a, gpointer b, const char *s) { (void)a; (void)b; (void)s; dwp_show_add_chat(NULL, NULL, NULL, NULL); }
static void menu_add_group(gpointer a, gpointer b, const char *s) { (void)a; (void)b; (void)s; dwp_show_add_group(); }
static void menu_quit(gpointer a, gpointer b, const char *s) { (void)a; (void)b; (void)s; dwp_core_request_quit(); }
static void menu_accounts(gpointer a, gpointer b, const char *s) { (void)a; (void)b; (void)s; dwp_accounts_show(); }
static void menu_account_actions(gpointer a, gpointer b, const char *s) { (void)a; (void)b; (void)s; dwp_accounts_popup(bl.window); }
static void menu_pounces(gpointer a, gpointer b, const char *s) { (void)a; (void)b; (void)s; dwp_pounces_show(); }
static void menu_privacy(gpointer a, gpointer b, const char *s) { (void)a; (void)b; (void)s; dwp_privacy_show(NULL); }
static void menu_plugins(gpointer a, gpointer b, const char *s) { (void)a; (void)b; (void)s; dwp_plugins_show(); }
static void menu_prefs(gpointer a, gpointer b, const char *s) { (void)a; (void)b; (void)s; dwp_prefs_show(); }
static void menu_xfers(gpointer a, gpointer b, const char *s) { (void)a; (void)b; (void)s; dwp_xfers_show(); }
static void menu_debug(gpointer a, gpointer b, const char *s) { (void)a; (void)b; (void)s; dwp_debug_show(); }

static void menu_show_offline(gpointer a, gpointer b, const char *s)
{
    gboolean on = !purple_prefs_get_bool(DWP_PREF_OFFLINE);

    (void)a; (void)b; (void)s;
    purple_prefs_set_bool(DWP_PREF_OFFLINE, on);
    dwp_menu_set_state(bl.buddies_menu, DWP_MID_SHOW_OFFLINE, on ? DW_MIS_CHECKED : DW_MIS_UNCHECKED);
    schedule_rebuild();
}

static void menu_show_empty(gpointer a, gpointer b, const char *s)
{
    gboolean on = !purple_prefs_get_bool(DWP_PREF_EMPTY);

    (void)a; (void)b; (void)s;
    purple_prefs_set_bool(DWP_PREF_EMPTY, on);
    dwp_menu_set_state(bl.buddies_menu, DWP_MID_SHOW_EMPTY, on ? DW_MIS_CHECKED : DW_MIS_UNCHECKED);
    schedule_rebuild();
}

static void menu_mute(gpointer a, gpointer b, const char *s)
{
    (void)a; (void)b; (void)s;
    dwp_sound_set_muted(!dwp_sound_muted());
    dwp_blist_sync_mute();
}

void dwp_blist_sync_mute(void)
{
    if(bl.tools_menu)
        dwp_menu_set_state(bl.tools_menu, DWP_MID_MUTE,
                           dwp_sound_muted() ? DW_MIS_CHECKED : DW_MIS_UNCHECKED);
}

static void menu_about(gpointer a, gpointer b, const char *s)
{
    DWEnv env;
    char *text;

    (void)a; (void)b; (void)s;
    dw_environment_query(&env);
    text = g_strdup_printf("%s %s\n\nAn instant messenger built on libpurple %s and "
                           "Dynamic Windows %d.%d.%d (%s).\n\nLicensed under the GNU GPL v2 or later.",
                           DWP_NAME, DWP_VERSION, purple_core_get_version(),
                           env.DWMajorVersion, env.DWMinorVersion, env.DWSubVersion, env.osName);
    dwp_message("About " DWP_NAME, text, NULL);
    g_free(text);
}

static void build_menubar(void)
{
    HMENUI menu;

    bl.menubar = dw_menubar_new(bl.window);

    menu = dw_menu_new(0);
    bl.buddies_menu = menu;
    dwp_menu_add_static(menu, "New ~Instant Message...", 0, FALSE, menu_new_im, NULL);
    dwp_menu_add_static(menu, "Join a ~Chat...", 0, FALSE, menu_join_chat, NULL);
    dwp_menu_add_static(menu, "Get User ~Info...", 0, FALSE, menu_get_info, NULL);
    dwp_menu_add_static(menu, "~Room List...", 0, FALSE, menu_roomlist, NULL);
    dwp_menu_separator(menu);
    dwp_menu_add_static(menu, "Show ~Offline Buddies", DWP_MID_SHOW_OFFLINE, TRUE, menu_show_offline, NULL);
    dwp_menu_add_static(menu, "Show ~Empty Groups", DWP_MID_SHOW_EMPTY, TRUE, menu_show_empty, NULL);
    dwp_menu_separator(menu);
    dwp_menu_add_static(menu, "~Add Buddy...", 0, FALSE, menu_add_buddy, NULL);
    dwp_menu_add_static(menu, "Add C~hat...", 0, FALSE, menu_add_chat, NULL);
    dwp_menu_add_static(menu, "Add ~Group...", 0, FALSE, menu_add_group, NULL);
    dwp_menu_separator(menu);
    dwp_menu_add_static(menu, "~Quit", 0, FALSE, menu_quit, NULL);
    dw_menu_append_item(bl.menubar, "~Buddies", DW_MENU_AUTO, 0, TRUE, FALSE, menu);

    menu = dw_menu_new(0);
    dwp_menu_add_static(menu, "~Manage Accounts", 0, FALSE, menu_accounts, NULL);
    dwp_menu_add_static(menu, "Account ~Actions...", 0, FALSE, menu_account_actions, NULL);
    dw_menu_append_item(bl.menubar, "~Accounts", DW_MENU_AUTO, 0, TRUE, FALSE, menu);

    menu = dw_menu_new(0);
    bl.tools_menu = menu;
    dwp_menu_add_static(menu, "Buddy ~Pounces", 0, FALSE, menu_pounces, NULL);
    dwp_menu_add_static(menu, "Pri~vacy", 0, FALSE, menu_privacy, NULL);
    dwp_menu_add_static(menu, "Plu~gins", 0, FALSE, menu_plugins, NULL);
    dwp_menu_add_static(menu, "Pr~eferences", 0, FALSE, menu_prefs, NULL);
    dwp_menu_add_static(menu, "File ~Transfers", 0, FALSE, menu_xfers, NULL);
    dwp_menu_separator(menu);
    dwp_menu_add_static(menu, "~Mute Sounds", DWP_MID_MUTE, TRUE, menu_mute, NULL);
    dwp_menu_add_static(menu, "~Debug Window", 0, FALSE, menu_debug, NULL);
    dw_menu_append_item(bl.menubar, "~Tools", DW_MENU_AUTO, 0, TRUE, FALSE, menu);

    menu = dw_menu_new(0);
    dwp_menu_add_static(menu, "~About", 0, FALSE, menu_about, NULL);
    dw_menu_append_item(bl.menubar, "~Help", DW_MENU_AUTO, 0, TRUE, FALSE, menu);
}

/* ================================================================ window */

static int DWSIGNAL blist_delete(HWND w, void *data)
{
    (void)w;
    (void)data;
    dwp_blist_save_geometry();
    /* With a tray icon the buddy list only hides; Quit exits. */
    if(dwp_tray_shown())
    {
        dw_window_hide(bl.window);
        bl.visible = FALSE;
    }
    else
        dwp_core_request_quit();
    return TRUE;
}

static int DWSIGNAL tray_button(HWND w, int x, int y, int button, void *data);

void dwp_blist_create(void)
{
    HWND box, row;
    int z;
    unsigned long flags[1] = { DW_CFA_STRING | DW_CFA_LEFT | DW_CFA_HORZSEPARATOR | DW_CFA_SEPARATOR };
    char *titles[1] = { "Status" };
    char *title;

    if(bl.window)
        return;
    bl.shown = g_hash_table_new(g_direct_hash, g_direct_equal);
    bl.expanded = g_hash_table_new(g_direct_hash, g_direct_equal);
    for(z = 0; z < N_STATUS; z++)
        status_labels[z] = (char *)dwp_primitive_label(status_prims[z]);

    title = g_strdup_printf("Buddy List - %s", DWP_NAME);
    bl.window = dwp_window_new(title, 300, 560);
    g_free(title);
    build_menubar();

    box = dw_box_new(DW_VERT, 2);
    dw_box_pack_start(bl.window, box, 0, 0, TRUE, TRUE, 0);

    bl.list = dw_container_new(0, FALSE);
    dw_filesystem_set_column_title(bl.list, "Buddy");
    dw_filesystem_setup(bl.list, flags, titles, 1);
    dw_box_pack_start(box, bl.list, 250, 400, TRUE, TRUE, 2);
    dw_signal_connect(bl.list, DW_SIGNAL_ITEM_ENTER, DW_SIGNAL_FUNC(list_enter), NULL);
    dw_signal_connect(bl.list, DW_SIGNAL_ITEM_CONTEXT, DW_SIGNAL_FUNC(list_context), NULL);

    row = dw_box_new(DW_HORZ, 0);
    dw_box_pack_start(box, row, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 1);
    bl.status_msg = dw_entryfield_new("", 0);
    dw_box_pack_start(row, bl.status_msg, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 1);
    {
        HWND set = dwp_button(row, "Set", DW_SIGNAL_FUNC(status_msg_set), NULL);

        dw_window_set_tooltip(set, "Set the status message");
        dw_window_set_tooltip(bl.status_msg, "Status message (press Enter to set)");
        dw_window_click_default(bl.status_msg, set);
    }

    bl.status_combo = dw_combobox_new(status_labels[0], 0);
    for(z = 0; z < N_STATUS; z++)
        dw_listbox_append(bl.status_combo, status_labels[z]);
    dw_box_pack_start(box, bl.status_combo, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 1);
    dw_signal_connect(bl.status_combo, DW_SIGNAL_LIST_SELECT, DW_SIGNAL_FUNC(status_selected), NULL);

    bl.status_bar = dw_status_text_new("", 0);
    dw_box_pack_start(box, bl.status_bar, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 1);

    /* Invisible anchor that receives tray icon clicks. */
    bl.tray_anchor = dw_render_new(0);
    dw_box_pack_start(box, bl.tray_anchor, 1, 1, FALSE, FALSE, 0);
    dw_signal_connect(bl.tray_anchor, DW_SIGNAL_BUTTON_PRESS, DW_SIGNAL_FUNC(tray_button), NULL);

    dw_signal_connect(bl.window, DW_SIGNAL_DELETE, DW_SIGNAL_FUNC(blist_delete), NULL);
    /* Application-level close (macOS); ignored elsewhere. */
    dw_signal_connect(DW_DESKTOP, DW_SIGNAL_DELETE, DW_SIGNAL_FUNC(blist_delete), NULL);

    purple_signal_connect(purple_savedstatuses_get_handle(), "savedstatus-changed", &blist_handle,
                          PURPLE_CALLBACK(on_savedstatus_changed), NULL);
}

void dwp_blist_apply_prefs(void)
{
    int w = purple_prefs_get_int(DWP_PREF_BLIST_W), h = purple_prefs_get_int(DWP_PREF_BLIST_H);

    if(w > 100 && h > 100)
        dw_window_set_size(bl.window, w, h);
    dwp_menu_set_state(bl.buddies_menu, DWP_MID_SHOW_OFFLINE,
                       purple_prefs_get_bool(DWP_PREF_OFFLINE) ? DW_MIS_CHECKED : DW_MIS_UNCHECKED);
    dwp_menu_set_state(bl.buddies_menu, DWP_MID_SHOW_EMPTY,
                       purple_prefs_get_bool(DWP_PREF_EMPTY) ? DW_MIS_CHECKED : DW_MIS_UNCHECKED);
    dwp_blist_sync_mute();
    sync_status_box();
    schedule_rebuild();
}

void dwp_blist_save_geometry(void)
{
    long x = 0, y = 0;
    unsigned long w = 0, h = 0;

    if(!bl.window || !bl.visible)
        return;
    dw_window_get_pos_size(bl.window, &x, &y, &w, &h);
    if(w > 100 && h > 100)
    {
        purple_prefs_set_int(DWP_PREF_BLIST_W, (int)w);
        purple_prefs_set_int(DWP_PREF_BLIST_H, (int)h);
    }
}

void dwp_blist_present(void)
{
    if(!bl.window)
        return;
    dw_window_show(bl.window);
    dw_window_raise(bl.window);
    bl.visible = TRUE;
}

void dwp_blist_toggle(void)
{
    if(bl.visible)
    {
        dwp_blist_save_geometry();
        dw_window_hide(bl.window);
        bl.visible = FALSE;
    }
    else
        dwp_blist_present();
}

gboolean dwp_blist_visible(void)
{
    return bl.visible;
}

/* Tray anchor clicks are forwarded to tray.c. */
static int DWSIGNAL tray_button(HWND w, int x, int y, int button, void *data)
{
    (void)w;
    (void)x;
    (void)y;
    (void)data;
    dwp_tray_clicked(button);
    return TRUE;
}

HWND dwp_blist_tray_anchor(void)
{
    return bl.tray_anchor;
}

HWND dwp_blist_window(void)
{
    return bl.window;
}

/* ================================================================ ui ops */

static void ui_new_list(PurpleBuddyList *list)
{
    (void)list;
}

static void ui_new_node(PurpleBlistNode *node)
{
    (void)node;
}

static void ui_show(PurpleBuddyList *list)
{
    (void)list;
    dwp_blist_present();
    schedule_rebuild();
}

static void ui_update(PurpleBuddyList *list, PurpleBlistNode *node)
{
    (void)list;
    (void)node;
    schedule_rebuild();
}

static void ui_remove(PurpleBuddyList *list, PurpleBlistNode *node)
{
    (void)list;
    if(bl.expanded)
        g_hash_table_remove(bl.expanded, node);
    if(bl.dead)
        return;
    /* The node is freed right after this returns: no row may refer to it. */
    if(bl.shown && g_hash_table_lookup(bl.shown, node))
        clear_list();
    schedule_rebuild();
}

static void ui_destroy(PurpleBuddyList *list)
{
    (void)list;
    bl.dead = TRUE;
    clear_list();
    purple_signals_disconnect_by_handle(&blist_handle);
}

static void ui_set_visible(PurpleBuddyList *list, gboolean show)
{
    (void)list;
    if(show)
        dwp_blist_present();
    else if(bl.window)
    {
        dw_window_hide(bl.window);
        bl.visible = FALSE;
    }
}

static void ui_request_add_buddy(PurpleAccount *account, const char *username,
                                 const char *group, const char *alias)
{
    dwp_show_add_buddy(account, username, group, alias);
}

static void ui_request_add_chat(PurpleAccount *account, PurpleGroup *group,
                                const char *alias, const char *name)
{
    dwp_show_add_chat(account, group ? purple_group_get_name(group) : NULL, alias, name);
}

static void ui_request_add_group(void)
{
    dwp_show_add_group();
}

PurpleBlistUiOps *dwp_blist_ui_ops(void)
{
    static PurpleBlistUiOps ops;
    static gboolean ready = FALSE;

    if(!ready)
    {
        memset(&ops, 0, sizeof(ops));
        ops.new_list = ui_new_list;
        ops.new_node = ui_new_node;
        ops.show = ui_show;
        ops.update = ui_update;
        ops.remove = ui_remove;
        ops.destroy = ui_destroy;
        ops.set_visible = ui_set_visible;
        ops.request_add_buddy = ui_request_add_buddy;
        ops.request_add_chat = ui_request_add_chat;
        ops.request_add_group = ui_request_add_group;
        /* save_node/remove_node/save_account left NULL: libpurple then
         * writes blist.xml itself. */
        ready = TRUE;
    }
    return &ops;
}

/* ===================================================== add buddy dialog */

typedef struct {
    HWND window, name, alias, group, invite;
    DwpAccountCombo *account;
} AddBuddyDlg;

static void add_buddy_free(gpointer p)
{
    AddBuddyDlg *d = p;

    dwp_account_combo_free(d->account);
    g_free(d);
}

static int DWSIGNAL add_buddy_cancel(HWND w, void *data)
{
    AddBuddyDlg *d = data;

    (void)w;
    dwp_window_close_later(d->window, add_buddy_free, d);
    return TRUE;
}

static int DWSIGNAL add_buddy_ok(HWND w, void *data)
{
    AddBuddyDlg *d = data;
    PurpleAccount *account = dwp_account_combo_get(d->account);
    char *name = dwp_get_text_trimmed(d->name);
    char *alias = dwp_get_text_trimmed(d->alias);
    char *groupname = dwp_get_text_trimmed(d->group);
    char *invite = dwp_get_text_trimmed(d->invite);
    PurpleGroup *group;
    PurpleBuddy *buddy;

    (void)w;
    if(!account || !purple_account_is_connected(account))
    {
        dw_messagebox("Add Buddy", DW_MB_OK | DW_MB_WARNING, "Please choose a connected account.");
        goto out;
    }
    if(!*name)
    {
        dw_messagebox("Add Buddy", DW_MB_OK | DW_MB_WARNING, "Please enter the buddy's username.");
        goto out;
    }
    group = dwp_group_find_or_create(groupname);
    buddy = purple_find_buddy_in_group(account, name, group);
    if(!buddy)
    {
        buddy = purple_buddy_new(account, name, *alias ? alias : NULL);
        purple_blist_add_buddy(buddy, NULL, group, NULL);
    }
    else if(*alias)
        purple_blist_alias_buddy(buddy, alias);
    purple_account_add_buddy_with_invite(account, buddy, *invite ? invite : NULL);
    dwp_window_close_later(d->window, add_buddy_free, d);
out:
    g_free(name);
    g_free(alias);
    g_free(groupname);
    g_free(invite);
    return TRUE;
}

void dwp_show_add_buddy(PurpleAccount *account, const char *user, const char *group, const char *alias)
{
    AddBuddyDlg *d = g_new0(AddBuddyDlg, 1);
    HWND box, buttons, okb;

    d->window = dwp_window_new("Add Buddy", 0, 0);
    box = dw_box_new(DW_VERT, 4);
    dw_box_pack_start(d->window, box, 0, 0, TRUE, TRUE, 0);
    d->account = dwp_account_combo_new(TRUE, NULL);
    if(account)
        dwp_account_combo_set(d->account, account);
    dwp_form_row(box, "Account:", dwp_account_combo_widget(d->account));
    d->name = dw_entryfield_new(user ? user : "", 0);
    dwp_form_row(box, "Buddy's username:", d->name);
    d->alias = dw_entryfield_new(alias ? alias : "", 0);
    dwp_form_row(box, "Alias (optional):", d->alias);
    d->group = dwp_group_combo_new(group);
    dwp_form_row(box, "Group:", d->group);
    d->invite = dw_entryfield_new("", 0);
    dwp_form_row(box, "Invite message:", d->invite);
    buttons = dw_box_new(DW_HORZ, 0);
    dw_box_pack_start(box, buttons, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 2);
    dwp_spacer(buttons);
    okb = dwp_button(buttons, "Add", DW_SIGNAL_FUNC(add_buddy_ok), d);
    dwp_button(buttons, "Cancel", DW_SIGNAL_FUNC(add_buddy_cancel), d);
    dw_signal_connect(d->window, DW_SIGNAL_DELETE, DW_SIGNAL_FUNC(add_buddy_cancel), d);
    dw_window_click_default(d->name, okb);
    dwp_window_present(d->window, 440, 240);
    dw_window_set_focus(user && *user ? d->alias : d->name);
}

/* =========================================================== chat dialog */

typedef struct {
    char *identifier, *label;
    gboolean required, is_int;
    HWND widget;
} ChatField;

typedef struct {
    gboolean join;
    HWND window, fields_holder, fields_box, alias, group;
    DwpAccountCombo *account;
    GPtrArray *fields;
    char *initial_name;
} ChatDlg;

static void chat_field_free(gpointer p)
{
    ChatField *f = p;

    g_free(f->identifier);
    g_free(f->label);
    g_free(f);
}

static void chat_dlg_free(gpointer p)
{
    ChatDlg *d = p;

    dwp_account_combo_free(d->account);
    g_ptr_array_free(d->fields, TRUE);
    g_free(d->initial_name);
    g_free(d);
}

static void chat_dlg_rebuild(gpointer data, PurpleAccount *account)
{
    ChatDlg *d = data;
    PurpleConnection *gc;
    PurplePluginProtocolInfo *info;
    GHashTable *defaults = NULL;
    GList *entries, *l;

    g_ptr_array_set_size(d->fields, 0);
    d->fields_box = dwp_box_reset(d->fields_holder, d->fields_box, DW_VERT);

    info = dwp_account_prpl_info(account, &gc);
    if(!gc || !info || !info->chat_info)
    {
        dwp_form_row(d->fields_box, "", dwp_label("Choose a connected account that supports chats."));
        return;
    }
    if(info->chat_info_defaults)
        defaults = info->chat_info_defaults(gc, d->initial_name);
    entries = info->chat_info(gc);
    for(l = entries; l; l = l->next)
    {
        struct proto_chat_entry *pce = l->data;
        const char *def = defaults ? g_hash_table_lookup(defaults, pce->identifier) : NULL;
        ChatField *f = g_new0(ChatField, 1);
        char *label, *stripped;

        f->identifier = g_strdup(pce->identifier);
        f->label = dwp_strip_mnemonic(pce->label);
        f->required = pce->required;
        f->is_int = pce->is_int;
        if(pce->is_int)
        {
            char buf[32];

            g_snprintf(buf, sizeof(buf), "%d", def ? atoi(def) : pce->min);
            f->widget = dw_spinbutton_new(buf, 0);
            dw_spinbutton_set_limits(f->widget, pce->max > pce->min ? pce->max : G_MAXINT, pce->min);
            dw_spinbutton_set_pos(f->widget, def ? atoi(def) : pce->min);
        }
        else if(pce->secret)
            f->widget = dw_entryfield_password_new(def ? def : "", 0);
        else
            f->widget = dw_entryfield_new(def ? def : "", 0);
        stripped = dwp_strip_mnemonic(pce->label);
        label = g_strdup_printf("%s%s", stripped, pce->required ? " *" : "");
        dwp_form_row(d->fields_box, label, f->widget);
        g_free(label);
        g_free(stripped);
        g_ptr_array_add(d->fields, f);
    }
    g_list_free_full(entries, g_free);
    if(defaults)
        g_hash_table_destroy(defaults);
    dw_window_redraw(d->window);
}

static GHashTable *chat_collect(ChatDlg *d)
{
    GHashTable *components = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    guint z;

    for(z = 0; z < d->fields->len; z++)
    {
        ChatField *f = g_ptr_array_index(d->fields, z);
        char *value;

        if(f->is_int)
            value = g_strdup_printf("%ld", dw_spinbutton_get_pos(f->widget));
        else
            value = dwp_get_text_trimmed(f->widget);
        if(!*value)
        {
            g_free(value);
            if(f->required)
            {
                dw_messagebox(d->join ? "Join Chat" : "Add Chat", DW_MB_OK | DW_MB_WARNING,
                              "\"%s\" is required.", f->label);
                g_hash_table_destroy(components);
                return NULL;
            }
            continue;
        }
        g_hash_table_replace(components, g_strdup(f->identifier), value);
    }
    return components;
}

static int DWSIGNAL chat_cancel(HWND w, void *data)
{
    ChatDlg *d = data;

    (void)w;
    dwp_window_close_later(d->window, chat_dlg_free, d);
    return TRUE;
}

static int DWSIGNAL chat_ok(HWND w, void *data)
{
    ChatDlg *d = data;
    PurpleAccount *account = dwp_account_combo_get(d->account);
    PurpleConnection *gc = account ? purple_account_get_connection(account) : NULL;
    GHashTable *components;

    (void)w;
    if(!gc)
    {
        dw_messagebox(d->join ? "Join Chat" : "Add Chat", DW_MB_OK | DW_MB_WARNING,
                      "Please choose a connected account.");
        return TRUE;
    }
    components = chat_collect(d);
    if(!components)
        return TRUE;
    if(d->join)
    {
        serv_join_chat(gc, components);
        g_hash_table_destroy(components);
    }
    else
    {
        char *alias = dwp_get_text_trimmed(d->alias);
        char *groupname = dwp_get_text_trimmed(d->group);
        PurpleChat *chat = purple_chat_new(account, *alias ? alias : NULL, components);

        if(!chat)
        {
            /* purple_chat_new() does not take ownership when it fails. */
            g_hash_table_destroy(components);
            dw_messagebox("Add Chat", DW_MB_OK | DW_MB_WARNING, "The chat could not be created.");
            g_free(alias);
            g_free(groupname);
            return TRUE;
        }
        purple_blist_add_chat(chat, dwp_group_find_or_create(groupname), NULL);
        g_free(alias);
        g_free(groupname);
    }
    dwp_window_close_later(d->window, chat_dlg_free, d);
    return TRUE;
}

static int DWSIGNAL chat_roomlist(HWND w, void *data)
{
    ChatDlg *d = data;

    (void)w;
    dwp_roomlist_show(dwp_account_combo_get(d->account), FALSE);
    return TRUE;
}

static gboolean chat_capable(PurpleAccount *account)
{
    PurplePluginProtocolInfo *info = dwp_account_prpl_info(account, NULL);

    return info && info->chat_info && info->join_chat;
}

static void show_chat_dialog(gboolean join, PurpleAccount *account, const char *group,
                             const char *alias, const char *name)
{
    ChatDlg *d = g_new0(ChatDlg, 1);
    HWND box, buttons;

    d->join = join;
    d->fields = g_ptr_array_new_with_free_func(chat_field_free);
    d->initial_name = g_strdup(name);
    d->window = dwp_window_new(join ? "Join a Chat" : "Add Chat", 0, 0);
    box = dw_box_new(DW_VERT, 4);
    dw_box_pack_start(d->window, box, 0, 0, TRUE, TRUE, 0);
    d->account = dwp_account_combo_new(TRUE, chat_capable);
    if(account)
        dwp_account_combo_set(d->account, account);
    dwp_form_row(box, "Account:", dwp_account_combo_widget(d->account));
    d->fields_holder = dw_box_new(DW_VERT, 0);
    dw_box_pack_start(box, d->fields_holder, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, TRUE, 0);
    if(!join)
    {
        d->alias = dw_entryfield_new(alias ? alias : "", 0);
        dwp_form_row(box, "Alias:", d->alias);
        d->group = dwp_group_combo_new(group);
        dwp_form_row(box, "Group:", d->group);
    }
    buttons = dw_box_new(DW_HORZ, 0);
    dw_box_pack_start(box, buttons, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 2);
    dwp_spacer(buttons);
    if(join)
        dwp_button(buttons, "Room List...", DW_SIGNAL_FUNC(chat_roomlist), d);
    dwp_button(buttons, join ? "Join" : "Add", DW_SIGNAL_FUNC(chat_ok), d);
    dwp_button(buttons, "Cancel", DW_SIGNAL_FUNC(chat_cancel), d);
    dw_signal_connect(d->window, DW_SIGNAL_DELETE, DW_SIGNAL_FUNC(chat_cancel), d);
    dwp_account_combo_on_change(d->account, chat_dlg_rebuild, d);
    chat_dlg_rebuild(d, dwp_account_combo_get(d->account));
    dwp_window_present(d->window, 440, join ? 240 : 300);
}

void dwp_show_add_chat(PurpleAccount *account, const char *group, const char *alias, const char *name)
{
    show_chat_dialog(FALSE, account, group, alias, name);
}

void dwp_show_join_chat(void)
{
    show_chat_dialog(TRUE, NULL, NULL, NULL, NULL);
}

static void add_group_done(gpointer data, const char *text)
{
    char *name = g_strstrip(g_strdup(text ? text : ""));

    (void)data;
    if(*name && !purple_find_group(name))
    {
        PurpleGroup *group = purple_group_new(name);

        purple_blist_add_group(group, NULL);
    }
    if(*name && purple_prefs_get_bool(DWP_PREF_EMPTY) == FALSE)
        dwp_blist_status_message("Empty groups are hidden; enable Buddies > Show Empty Groups to see it.");
    g_free(name);
}

void dwp_show_add_group(void)
{
    dwp_prompt("Add Group", "Group name:", "", FALSE, add_group_done, NULL);
}

/* ============================================= account + name dialogs */

typedef void (*NameFunc)(PurpleAccount *account, const char *name);

typedef struct {
    HWND window, name;
    DwpAccountCombo *account;
    NameFunc func;
} NameDlg;

static void name_dlg_free(gpointer p)
{
    NameDlg *d = p;

    dwp_account_combo_free(d->account);
    g_free(d);
}

static int DWSIGNAL name_cancel(HWND w, void *data)
{
    NameDlg *d = data;

    (void)w;
    dwp_window_close_later(d->window, name_dlg_free, d);
    return TRUE;
}

static int DWSIGNAL name_ok(HWND w, void *data)
{
    NameDlg *d = data;
    PurpleAccount *account = dwp_account_combo_get(d->account);
    char *name = dwp_get_text_trimmed(d->name);

    (void)w;
    if(!account || !purple_account_is_connected(account) || !*name)
    {
        dw_messagebox(DWP_NAME, DW_MB_OK | DW_MB_WARNING, "Please choose a connected account and enter a name.");
        g_free(name);
        return TRUE;
    }
    d->func(account, name);
    g_free(name);
    dwp_window_close_later(d->window, name_dlg_free, d);
    return TRUE;
}

static void show_name_dialog(const char *title, NameFunc func)
{
    NameDlg *d = g_new0(NameDlg, 1);
    HWND box, buttons, okb;

    d->func = func;
    d->window = dwp_window_new(title, 0, 0);
    box = dw_box_new(DW_VERT, 4);
    dw_box_pack_start(d->window, box, 0, 0, TRUE, TRUE, 0);
    d->account = dwp_account_combo_new(TRUE, NULL);
    dwp_form_row(box, "Account:", dwp_account_combo_widget(d->account));
    d->name = dw_entryfield_new("", 0);
    dwp_form_row(box, "Name:", d->name);
    buttons = dw_box_new(DW_HORZ, 0);
    dw_box_pack_start(box, buttons, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 2);
    dwp_spacer(buttons);
    okb = dwp_button(buttons, "OK", DW_SIGNAL_FUNC(name_ok), d);
    dwp_button(buttons, "Cancel", DW_SIGNAL_FUNC(name_cancel), d);
    dw_signal_connect(d->window, DW_SIGNAL_DELETE, DW_SIGNAL_FUNC(name_cancel), d);
    dw_window_click_default(d->name, okb);
    dwp_window_present(d->window, 420, 140);
    dw_window_set_focus(d->name);
}

static void new_im_cb(PurpleAccount *account, const char *name)
{
    dwp_open_im(account, name);
}

static void get_info_cb(PurpleAccount *account, const char *name)
{
    PurpleConnection *gc = purple_account_get_connection(account);

    if(gc)
        serv_get_info(gc, name);
}

void dwp_show_new_im(void)
{
    show_name_dialog("New Instant Message", new_im_cb);
}

void dwp_show_get_info(void)
{
    show_name_dialog("Get User Info", get_info_cb);
}
