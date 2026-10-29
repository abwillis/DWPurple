/*
 * DWPurple - plugin manager, plugin actions and plugin preference frames.
 */
#include "dwpurple.h"

typedef struct {
    HWND window, list, details;
} PluginsDlg;

static PluginsDlg *pd = NULL;
static GPtrArray *plugin_actions = NULL;

static gboolean plugin_listed(PurplePlugin *plugin)
{
    const char *ui;

    if(!plugin || !plugin->info || plugin->info->type != PURPLE_PLUGIN_STANDARD)
        return FALSE;
    if(plugin->info->flags & PURPLE_PLUGIN_FLAG_INVISIBLE)
        return FALSE;
    ui = plugin->info->ui_requirement;
    return !ui || strcmp(ui, DWP_UI_ID) == 0;
}

static gboolean plugin_exists(PurplePlugin *plugin)
{
    return plugin && g_list_find(purple_plugins_get_all(), plugin) != NULL;
}

static PurplePlugin *selected_plugin(void)
{
    PurplePlugin *p;

    if(!pd)
        return NULL;
    p = (PurplePlugin *)dw_container_query_start(pd->list, DW_CRA_SELECTED | DW_CR_RETDATA);
    return plugin_exists(p) ? p : NULL;
}

static void show_details(PurplePlugin *p)
{
    GString *s;
    PurplePluginInfo *info;

    if(!pd)
        return;
    dw_mle_clear(pd->details);
    if(!p)
        return;
    info = p->info;
    s = g_string_new(NULL);
    g_string_append_printf(s, "%s %s\r\n%s\r\n", info->name ? info->name : "",
                           info->version ? info->version : "", info->description ? info->description : "");
    if(info->author)
        g_string_append_printf(s, "\r\nWritten by: %s", info->author);
    if(info->homepage)
        g_string_append_printf(s, "\r\nWeb site: %s", info->homepage);
    if(p->path)
    {
        char *path = dwp_filename_to_utf8(p->path);

        g_string_append_printf(s, "\r\nFilename: %s", path);
        g_free(path);
    }
    if(p->error)
        g_string_append_printf(s, "\r\n\r\nError: %s", p->error);
    dw_mle_import(pd->details, s->str, 0);
    dw_mle_set_cursor(pd->details, 0);
    g_string_free(s, TRUE);
}

static gint plugin_compare(gconstpointer a, gconstpointer b)
{
    const PurplePlugin *x = *(PurplePlugin *const *)a, *y = *(PurplePlugin *const *)b;

    return g_utf8_collate(x->info->name ? x->info->name : "", y->info->name ? y->info->name : "");
}

static void refresh(void)
{
    GPtrArray *list = g_ptr_array_new();
    PurplePlugin *selected = selected_plugin();
    GList *l;
    void *alloc;
    guint z;

    if(!pd)
        return;
    for(l = purple_plugins_get_all(); l; l = l->next)
        if(plugin_listed(l->data))
            g_ptr_array_add(list, l->data);
    g_ptr_array_sort(list, plugin_compare);
    dw_container_clear(pd->list, TRUE);
    if(list->len)
    {
        alloc = dw_container_alloc(pd->list, (int)list->len);
        for(z = 0; z < list->len; z++)
        {
            PurplePlugin *p = g_ptr_array_index(list, z);
            char *loaded = purple_plugin_is_loaded(p) ? "Yes" : (p->error ? "Error" : "No");
            char *name = p->info->name ? p->info->name : p->info->id;
            char *version = p->info->version ? p->info->version : "";
            char *summary = p->info->summary ? p->info->summary : "";

            dw_container_set_item(pd->list, alloc, 0, (int)z, &loaded);
            dw_container_set_item(pd->list, alloc, 1, (int)z, &name);
            dw_container_set_item(pd->list, alloc, 2, (int)z, &version);
            dw_container_set_item(pd->list, alloc, 3, (int)z, &summary);
            dw_container_set_row_title(alloc, (int)z, name);
            dw_container_set_row_data(alloc, (int)z, p);
        }
        dw_container_insert(pd->list, alloc, (int)list->len);
        dw_container_optimize(pd->list);
    }
    if(selected)
        dw_container_cursor_by_data(pd->list, selected);
    g_ptr_array_free(list, TRUE);
}

static int DWSIGNAL plugin_select(HWND w, HTREEITEM item, char *text, void *data, void *itemdata)
{
    (void)w;
    (void)item;
    (void)text;
    (void)data;
    show_details(plugin_exists(itemdata) ? itemdata : NULL);
    return FALSE;
}

static int DWSIGNAL plugin_toggle(HWND w, void *data)
{
    PurplePlugin *p = selected_plugin();
    gboolean want, ok;

    (void)w;
    (void)data;
    if(!p)
        return TRUE;
    want = !purple_plugin_is_loaded(p);
    ok = want ? purple_plugin_load(p) : purple_plugin_unload(p);
    if(!ok)
        dw_messagebox("Plugins", DW_MB_OK | DW_MB_WARNING, "%s could not be %s.%s%s",
                      p->info->name ? p->info->name : "The plugin", want ? "loaded" : "unloaded",
                      p->error ? "\n\n" : "", p->error ? p->error : "");
    purple_plugins_save_loaded(DWP_PREF_LOADED);
    refresh();
    show_details(plugin_exists(p) ? p : NULL);
    return TRUE;
}

static int DWSIGNAL plugin_enter(HWND w, char *text, void *data, void *itemdata)
{
    (void)text;
    (void)itemdata;
    return plugin_toggle(w, data);
}

/* -------------------------------------------------------- pref frames */

typedef struct {
    PurplePluginPref *pref;
    char *name;
    HWND widget;
    PurplePrefType ptype;
    gboolean multiline;
    GPtrArray *labels;
    GPtrArray *values;      /* gpointer values for choices */
} PrefWidget;

typedef struct {
    HWND window;
    PurplePluginPrefFrame *frame;
    GPtrArray *widgets;
} PrefsDlg;

static void pref_widget_free(gpointer p)
{
    PrefWidget *w = p;

    g_free(w->name);
    if(w->labels)
        g_ptr_array_free(w->labels, TRUE);
    if(w->values)
        g_ptr_array_free(w->values, TRUE);
    g_free(w);
}

static void prefs_dlg_free(gpointer p)
{
    PrefsDlg *d = p;

    g_ptr_array_free(d->widgets, TRUE);
    purple_plugin_pref_frame_destroy(d->frame);
    g_free(d);
}

static int DWSIGNAL prefs_close(HWND w, void *data)
{
    PrefsDlg *d = data;

    (void)w;
    dwp_window_close_later(d->window, prefs_dlg_free, d);
    return TRUE;
}

static int DWSIGNAL prefs_save(HWND w, void *data)
{
    PrefsDlg *d = data;
    guint z;

    for(z = 0; z < d->widgets->len; z++)
    {
        PrefWidget *pw = g_ptr_array_index(d->widgets, z);

        if(!purple_prefs_exists(pw->name))
            continue;
        if(pw->values)
        {
            int index = dwp_combo_index(pw->widget, (char **)pw->labels->pdata, (int)pw->labels->len);
            gpointer value;

            if(index < 0)
                continue;
            value = g_ptr_array_index(pw->values, index);
            if(pw->ptype == PURPLE_PREF_INT)
                purple_prefs_set_int(pw->name, GPOINTER_TO_INT(value));
            else if(pw->ptype == PURPLE_PREF_BOOLEAN)
                purple_prefs_set_bool(pw->name, GPOINTER_TO_INT(value) ? TRUE : FALSE);
            else
                purple_prefs_set_string(pw->name, value);
            continue;
        }
        switch(pw->ptype)
        {
            case PURPLE_PREF_BOOLEAN:
                purple_prefs_set_bool(pw->name, dw_checkbox_get(pw->widget) ? TRUE : FALSE);
                break;
            case PURPLE_PREF_INT:
                purple_prefs_set_int(pw->name, (int)dw_spinbutton_get_pos(pw->widget));
                break;
            case PURPLE_PREF_STRING:
            case PURPLE_PREF_PATH:
            {
                char *v = pw->multiline ? dwp_mle_get_text(pw->widget) : dwp_get_text(pw->widget);

                if(pw->ptype == PURPLE_PREF_PATH)
                    purple_prefs_set_path(pw->name, v);
                else
                    purple_prefs_set_string(pw->name, v);
                g_free(v);
                break;
            }
            default:
                break;
        }
    }
    return prefs_close(w, data);
}

static void show_pref_frame(PurplePlugin *plugin, PurplePluginPrefFrame *frame)
{
    PrefsDlg *d = g_new0(PrefsDlg, 1);
    HWND box, scroll, buttons;
    GList *l;
    char *title = g_strdup_printf("%s Preferences", plugin->info->name ? plugin->info->name : "Plugin");

    d->frame = frame;
    d->widgets = g_ptr_array_new_with_free_func(pref_widget_free);
    d->window = dwp_window_new(title, 0, 0);
    g_free(title);
    box = dw_box_new(DW_VERT, 4);
    dw_box_pack_start(d->window, box, 0, 0, TRUE, TRUE, 0);
    scroll = dw_scrollbox_new(DW_VERT, 2);
    dw_box_pack_start(box, scroll, 460, 300, TRUE, TRUE, 2);

    for(l = purple_plugin_pref_frame_get_prefs(frame); l; l = l->next)
    {
        PurplePluginPref *pref = l->data;
        const char *name = purple_plugin_pref_get_name(pref);
        char *label = dwp_strip_mnemonic(purple_plugin_pref_get_label(pref));
        PrefWidget *pw;

        if(!name || purple_plugin_pref_get_type(pref) == PURPLE_PLUGIN_PREF_INFO || !purple_prefs_exists(name))
        {
            dwp_text_block(scroll, label, 420, 160);
            g_free(label);
            continue;
        }
        pw = g_new0(PrefWidget, 1);
        pw->pref = pref;
        pw->name = g_strdup(name);
        pw->ptype = purple_prefs_get_type(name);
        if(purple_plugin_pref_get_type(pref) == PURPLE_PLUGIN_PREF_CHOICE)
        {
            GList *c = purple_plugin_pref_get_choices(pref);
            int sel = 0;

            pw->labels = g_ptr_array_new_with_free_func(g_free);
            pw->values = g_ptr_array_new();
            /* choices: label, value, label, value, ... */
            for(; c && c->next; c = c->next->next)
            {
                gpointer value = c->next->data;
                gboolean current = FALSE;

                if(pw->ptype == PURPLE_PREF_INT)
                    current = purple_prefs_get_int(name) == GPOINTER_TO_INT(value);
                else if(pw->ptype == PURPLE_PREF_BOOLEAN)
                    current = !!purple_prefs_get_bool(name) == !!GPOINTER_TO_INT(value);
                else
                    current = purple_strequal(purple_prefs_get_string(name), value);
                if(current)
                    sel = (int)pw->labels->len;
                g_ptr_array_add(pw->labels, dwp_strip_mnemonic(c->data));
                g_ptr_array_add(pw->values, value);
            }
            pw->widget = dw_combobox_new("", 0);
            dwp_combo_fill(pw->widget, (char **)pw->labels->pdata, (int)pw->labels->len, sel);
            dwp_form_row(scroll, label, pw->widget);
        }
        else switch(pw->ptype)
        {
            case PURPLE_PREF_BOOLEAN:
                pw->widget = dw_checkbox_new(label, 0);
                dw_checkbox_set(pw->widget, purple_prefs_get_bool(name));
                dwp_form_row(scroll, "", pw->widget);
                break;
            case PURPLE_PREF_INT:
            {
                int min = 0, max = 0;
                char buf[32];

                purple_plugin_pref_get_bounds(pref, &min, &max);
                if(max <= min)
                {
                    min = G_MININT;
                    max = G_MAXINT;
                }
                g_snprintf(buf, sizeof(buf), "%d", purple_prefs_get_int(name));
                pw->widget = dw_spinbutton_new(buf, 0);
                dw_spinbutton_set_limits(pw->widget, max, min);
                dw_spinbutton_set_pos(pw->widget, purple_prefs_get_int(name));
                dwp_form_row(scroll, label, pw->widget);
                break;
            }
            case PURPLE_PREF_STRING:
            case PURPLE_PREF_PATH:
            {
                const char *value = pw->ptype == PURPLE_PREF_PATH ? purple_prefs_get_path(name)
                                                                  : purple_prefs_get_string(name);

                pw->multiline = purple_plugin_pref_get_type(pref) == PURPLE_PLUGIN_PREF_STRING_FORMAT &&
                                (purple_plugin_pref_get_format_type(pref) & PURPLE_STRING_FORMAT_TYPE_MULTILINE);
                if(pw->multiline)
                {
                    pw->widget = dw_mle_new(0);
                    dw_mle_set_word_wrap(pw->widget, TRUE);
                    if(value)
                        dw_mle_import(pw->widget, value, 0);
                    dw_box_pack_start(scroll, dwp_label(label), DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 1);
                    dw_box_pack_start(scroll, pw->widget, 420, 90, TRUE, TRUE, 2);
                }
                else
                {
                    pw->widget = purple_plugin_pref_get_masked(pref)
                                 ? dw_entryfield_password_new(value ? value : "", 0)
                                 : dw_entryfield_new(value ? value : "", 0);
                    if(purple_plugin_pref_get_max_length(pref))
                        dw_entryfield_set_limit(pw->widget, purple_plugin_pref_get_max_length(pref));
                    dwp_form_row(scroll, label, pw->widget);
                }
                break;
            }
            default:
                g_free(pw->name);
                g_free(pw);
                pw = NULL;
                break;
        }
        if(pw)
            g_ptr_array_add(d->widgets, pw);
        g_free(label);
    }

    buttons = dw_box_new(DW_HORZ, 0);
    dw_box_pack_start(box, buttons, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 2);
    dwp_spacer(buttons);
    dwp_button(buttons, "Save", DW_SIGNAL_FUNC(prefs_save), d);
    dwp_button(buttons, "Close", DW_SIGNAL_FUNC(prefs_close), d);
    dw_signal_connect(d->window, DW_SIGNAL_DELETE, DW_SIGNAL_FUNC(prefs_close), d);
    dwp_window_present(d->window, 500, 400);
}

static int DWSIGNAL plugin_configure(HWND w, void *data)
{
    PurplePlugin *p = selected_plugin();
    PurplePluginPrefFrame *frame;

    (void)w;
    (void)data;
    if(!p || !purple_plugin_is_loaded(p))
    {
        dw_messagebox("Plugins", DW_MB_OK | DW_MB_INFORMATION, "Load the plugin first.");
        return TRUE;
    }
    if(!p->info->prefs_info || !p->info->prefs_info->get_plugin_pref_frame)
    {
        dw_messagebox("Plugins", DW_MB_OK | DW_MB_INFORMATION, "This plugin has no preferences.");
        return TRUE;
    }
    frame = p->info->prefs_info->get_plugin_pref_frame(p);
    if(frame)
        show_pref_frame(p, frame);
    return TRUE;
}

static void run_plugin_action(gpointer act, gpointer plugin, const char *s)
{
    PurplePluginAction *pa = act;

    (void)s;
    if(plugin_exists(plugin) && purple_plugin_is_loaded(plugin) && pa->callback)
        pa->callback(pa);
}

static int DWSIGNAL plugin_actions_popup(HWND w, void *data)
{
    PurplePlugin *p = selected_plugin();
    HMENUI menu;
    GList *actions, *a;
    gboolean any = FALSE;

    (void)w;
    (void)data;
    if(!p || !purple_plugin_is_loaded(p) || !PURPLE_PLUGIN_HAS_ACTIONS(p))
    {
        dw_messagebox("Plugins", DW_MB_OK | DW_MB_INFORMATION, "This plugin has no actions (or is not loaded).");
        return TRUE;
    }
    dwp_actions_reset();
    if(!plugin_actions)
        plugin_actions = g_ptr_array_new_with_free_func((GDestroyNotify)purple_plugin_action_free);
    g_ptr_array_set_size(plugin_actions, 0);
    menu = dw_menu_new(0);
    actions = PURPLE_PLUGIN_ACTIONS(p, NULL);
    for(a = actions; a; a = a->next)
    {
        PurplePluginAction *pa = a->data;
        char *label;

        if(!pa)
        {
            dwp_menu_separator(menu);
            continue;
        }
        pa->plugin = p;
        g_ptr_array_add(plugin_actions, pa);
        label = dwp_mnemonic(pa->label);
        dwp_menu_add(menu, label, run_plugin_action, pa, p, NULL);
        g_free(label);
        any = TRUE;
    }
    g_list_free(actions);
    if(any)
        dwp_menu_popup_here(&menu, pd->window);
    else
        dw_menu_destroy(&menu);
    return TRUE;
}

static int DWSIGNAL plugin_rescan(HWND w, void *data)
{
    (void)w;
    (void)data;
    purple_plugins_probe(G_MODULE_SUFFIX);
    refresh();
    return TRUE;
}

static void plugins_free(gpointer p)
{
    g_free(p);
}

static int DWSIGNAL plugins_close(HWND w, void *data)
{
    PluginsDlg *d = pd;

    (void)w;
    (void)data;
    if(!d)
        return TRUE;
    pd = NULL;
    dwp_window_close_later(d->window, plugins_free, d);
    return TRUE;
}

void dwp_plugins_show(void)
{
    unsigned long flags[4];
    char *titles[4] = { "Loaded", "Name", "Version", "Summary" };
    HWND box, buttons;
    int z;

    if(pd)
    {
        dw_window_show(pd->window);
        dw_window_raise(pd->window);
        return;
    }
    for(z = 0; z < 4; z++)
        flags[z] = DW_CFA_STRING | DW_CFA_LEFT | DW_CFA_HORZSEPARATOR | DW_CFA_SEPARATOR;
    pd = g_new0(PluginsDlg, 1);
    pd->window = dwp_window_new("Plugins", 0, 0);
    box = dw_box_new(DW_VERT, 4);
    dw_box_pack_start(pd->window, box, 0, 0, TRUE, TRUE, 0);
    pd->list = dw_container_new(0, FALSE);
    dw_container_setup(pd->list, flags, titles, 4, 0);
    dw_box_pack_start(box, pd->list, 560, 240, TRUE, TRUE, 2);
    dw_signal_connect(pd->list, DW_SIGNAL_ITEM_SELECT, DW_SIGNAL_FUNC(plugin_select), NULL);
    dw_signal_connect(pd->list, DW_SIGNAL_ITEM_ENTER, DW_SIGNAL_FUNC(plugin_enter), NULL);
    pd->details = dw_mle_new(0);
    dw_mle_set_editable(pd->details, FALSE);
    dw_mle_set_word_wrap(pd->details, TRUE);
    dw_box_pack_start(box, pd->details, 560, 110, TRUE, FALSE, 2);
    buttons = dw_box_new(DW_HORZ, 0);
    dw_box_pack_start(box, buttons, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 2);
    dwp_button(buttons, "Load/Unload", DW_SIGNAL_FUNC(plugin_toggle), NULL);
    dwp_button(buttons, "Configure...", DW_SIGNAL_FUNC(plugin_configure), NULL);
    dwp_button(buttons, "Actions...", DW_SIGNAL_FUNC(plugin_actions_popup), NULL);
    dwp_button(buttons, "Rescan", DW_SIGNAL_FUNC(plugin_rescan), NULL);
    dwp_spacer(buttons);
    dwp_button(buttons, "Close", DW_SIGNAL_FUNC(plugins_close), NULL);
    dw_signal_connect(pd->window, DW_SIGNAL_DELETE, DW_SIGNAL_FUNC(plugins_close), NULL);
    refresh();
    dwp_window_present(pd->window, 640, 470);
}
