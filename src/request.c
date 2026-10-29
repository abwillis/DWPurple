/*
 * DWPurple - libpurple request UI (input, choice, action, fields, file).
 *
 * Every request is a non-modal window.  complete() runs the libpurple
 * callback exactly once and then asks libpurple to close the request,
 * which comes back through close_request(); a request closed by libpurple
 * first never runs a callback.
 */
#include "dwpurple.h"

typedef struct {
    PurpleRequestField *field;
    HWND widget;
    GPtrArray *items;           /* list/choice labels */
    DwpAccountCombo *account;
    gboolean multiline;
} FieldWidget;

typedef struct {
    PurpleRequestType type;
    HWND window;
    gboolean done, closed, busy;
    void *user_data;
    GCallback ok_cb, cancel_cb;
    /* input */
    HWND entry;
    gboolean multiline;
    /* choice */
    HWND combo;
    GArray *choice_values;
    GPtrArray *choice_labels;
    /* action */
    GArray *action_cbs;
    GPtrArray *action_buttons;
    /* fields */
    PurpleRequestFields *fields;
    GPtrArray *field_widgets;
    /* file / folder */
    char *title, *filename;
    gboolean save;
} Req;

static GList *live = NULL;

static void field_widget_free(gpointer p)
{
    FieldWidget *fw = p;

    if(fw->items)
        g_ptr_array_free(fw->items, TRUE);
    if(fw->account)
        dwp_account_combo_free(fw->account);
    g_free(fw);
}

static void req_free(gpointer p)
{
    Req *r = p;

    if(r->busy)
    {
        /* A file dialog for this request is still open (nested loop). */
        dwp_defer(req_free, r);
        return;
    }
    if(r->choice_values)
        g_array_free(r->choice_values, TRUE);
    if(r->choice_labels)
        g_ptr_array_free(r->choice_labels, TRUE);
    if(r->action_cbs)
        g_array_free(r->action_cbs, TRUE);
    if(r->action_buttons)
        g_ptr_array_free(r->action_buttons, TRUE);
    if(r->field_widgets)
        g_ptr_array_free(r->field_widgets, TRUE);
    /* The UI owns the fields once request_fields() has been called. */
    if(r->fields)
        purple_request_fields_destroy(r->fields);
    g_free(r->title);
    g_free(r->filename);
    g_free(r);
}

static void close_from_core(Req *r)
{
    if(r->closed)
        return;
    r->closed = TRUE;
    r->done = TRUE;
    live = g_list_remove(live, r);
    dwp_window_close_later(r->window, req_free, r);
}

typedef void (*Invoke)(Req *r, gpointer arg);

/* Run the callback once, then let libpurple close the request. */
static void complete(Req *r, Invoke invoke, gpointer arg)
{
    if(r->done)
        return;
    r->done = TRUE;
    if(r->window)
        dw_window_hide(r->window);
    if(invoke)
        invoke(r, arg);
    purple_request_close(r->type, r);
    if(!r->closed)
        close_from_core(r);
}

static Req *req_new(PurpleRequestType type, void *user_data)
{
    Req *r = g_new0(Req, 1);

    r->type = type;
    r->user_data = user_data;
    live = g_list_append(live, r);
    return r;
}

static const char *dialog_title(const char *title)
{
    return title && *title ? title : DWP_NAME;
}

static void add_header(HWND box, const char *primary, const char *secondary, PurpleAccount *account)
{
    /* Read-only text blocks: always laid out, and the text (for example a
     * device authorization code) can be selected and copied. */
    if(primary && *primary)
        dwp_text_block(box, primary, 400, 120);
    if(secondary && *secondary)
        dwp_text_block(box, secondary, 400, 200);
    if(account)
    {
        char *name = dwp_account_display_name(account);
        char *text = g_strdup_printf("Account: %s", name);

        dw_box_pack_start(box, dwp_label(text), 400, DW_SIZE_AUTO, TRUE, FALSE, 3);
        g_free(text);
        g_free(name);
    }
}

static HWND window_box(Req *r, const char *title)
{
    HWND box;

    r->window = dwp_window_new(dialog_title(title), 0, 0);
    box = dw_box_new(DW_VERT, 4);
    dw_box_pack_start(r->window, box, 0, 0, TRUE, TRUE, 0);
    return box;
}

static HWND button_row(HWND box)
{
    HWND row = dw_box_new(DW_HORZ, 0);

    dw_box_pack_start(box, row, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 2);
    dwp_spacer(row);
    return row;
}

static char *button_text(const char *text, const char *fallback)
{
    return dwp_strip_mnemonic(text && *text ? text : fallback);
}

/* ================================================================== input */

static void invoke_input(Req *r, gpointer arg)
{
    GCallback cb = arg;

    if(cb)
        ((PurpleRequestInputCb)cb)(r->user_data, r->filename);
}

static void input_finish(Req *r, GCallback cb)
{
    if(r->done)
        return;
    /* Read the value before the window goes away. */
    g_free(r->filename);
    r->filename = r->multiline ? dwp_mle_get_text(r->entry) : dwp_get_text(r->entry);
    complete(r, invoke_input, (gpointer)cb);
}

static int DWSIGNAL input_ok(HWND w, void *data)
{
    Req *r = data;

    (void)w;
    input_finish(r, r->ok_cb);
    return TRUE;
}

static int DWSIGNAL input_cancel(HWND w, void *data)
{
    Req *r = data;

    (void)w;
    input_finish(r, r->cancel_cb);
    return TRUE;
}

static void *request_input(const char *title, const char *primary, const char *secondary,
                           const char *default_value, gboolean multiline, gboolean masked, gchar *hint,
                           const char *ok_text, GCallback ok_cb, const char *cancel_text, GCallback cancel_cb,
                           PurpleAccount *account, const char *who, PurpleConversation *conv, void *user_data)
{
    Req *r = req_new(PURPLE_REQUEST_INPUT, user_data);
    HWND box, buttons, okb;
    char *okt = button_text(ok_text, "OK"), *cancelt = button_text(cancel_text, "Cancel");

    (void)hint;
    (void)who;
    (void)conv;
    r->ok_cb = ok_cb;
    r->cancel_cb = cancel_cb;
    r->multiline = multiline;
    box = window_box(r, title);
    add_header(box, primary, secondary, account);
    if(multiline)
    {
        r->entry = dw_mle_new(0);
        dw_mle_set_word_wrap(r->entry, TRUE);
        if(default_value)
            dw_mle_import(r->entry, default_value, 0);
        dw_box_pack_start(box, r->entry, 400, 120, TRUE, TRUE, 3);
    }
    else
    {
        r->entry = masked ? dw_entryfield_password_new(default_value ? default_value : "", 0)
                          : dw_entryfield_new(default_value ? default_value : "", 0);
        dw_box_pack_start(box, r->entry, 400, DW_SIZE_AUTO, TRUE, FALSE, 3);
    }
    buttons = button_row(box);
    okb = dwp_button(buttons, okt, DW_SIGNAL_FUNC(input_ok), r);
    dwp_button(buttons, cancelt, DW_SIGNAL_FUNC(input_cancel), r);
    dw_signal_connect(r->window, DW_SIGNAL_DELETE, DW_SIGNAL_FUNC(input_cancel), r);
    if(!multiline)
        dw_window_click_default(r->entry, okb);
    dwp_window_present(r->window, 460, multiline ? 340 : 240);
    dw_window_set_focus(r->entry);
    g_free(okt);
    g_free(cancelt);
    return r;
}

/* ================================================================= choice */

static void invoke_choice(Req *r, gpointer arg)
{
    GCallback cb = arg;
    int index = dwp_combo_index(r->combo, (char **)r->choice_labels->pdata, (int)r->choice_labels->len);
    int value = index >= 0 ? g_array_index(r->choice_values, int, index) : 0;

    if(cb)
        ((PurpleRequestChoiceCb)cb)(r->user_data, value);
}

static int DWSIGNAL choice_ok(HWND w, void *data)
{
    Req *r = data;

    (void)w;
    complete(r, invoke_choice, (gpointer)r->ok_cb);
    return TRUE;
}

static int DWSIGNAL choice_cancel(HWND w, void *data)
{
    Req *r = data;

    (void)w;
    complete(r, invoke_choice, (gpointer)r->cancel_cb);
    return TRUE;
}

static void *request_choice(const char *title, const char *primary, const char *secondary, int default_value,
                            const char *ok_text, GCallback ok_cb, const char *cancel_text, GCallback cancel_cb,
                            PurpleAccount *account, const char *who, PurpleConversation *conv,
                            void *user_data, va_list choices)
{
    Req *r = req_new(PURPLE_REQUEST_CHOICE, user_data);
    HWND box, buttons;
    const char *label;
    int sel = 0;
    char *okt = button_text(ok_text, "OK"), *cancelt = button_text(cancel_text, "Cancel");

    (void)who;
    (void)conv;
    r->ok_cb = ok_cb;
    r->cancel_cb = cancel_cb;
    r->choice_values = g_array_new(FALSE, FALSE, sizeof(int));
    r->choice_labels = g_ptr_array_new_with_free_func(g_free);
    while((label = va_arg(choices, const char *)) != NULL)
    {
        int value = va_arg(choices, int);

        if(value == default_value)
            sel = (int)r->choice_values->len;
        g_array_append_val(r->choice_values, value);
        g_ptr_array_add(r->choice_labels, dwp_strip_mnemonic(label));
    }
    box = window_box(r, title);
    add_header(box, primary, secondary, account);
    r->combo = dw_combobox_new("", 0);
    dwp_combo_fill(r->combo, (char **)r->choice_labels->pdata, (int)r->choice_labels->len, sel);
    dw_box_pack_start(box, r->combo, 400, DW_SIZE_AUTO, TRUE, FALSE, 3);
    buttons = button_row(box);
    dwp_button(buttons, okt, DW_SIGNAL_FUNC(choice_ok), r);
    dwp_button(buttons, cancelt, DW_SIGNAL_FUNC(choice_cancel), r);
    dw_signal_connect(r->window, DW_SIGNAL_DELETE, DW_SIGNAL_FUNC(choice_cancel), r);
    dwp_window_present(r->window, 460, 240);
    g_free(okt);
    g_free(cancelt);
    return r;
}

/* ================================================================= action */

typedef struct {
    Req *r;
    int index;
} ActionButton;

static void invoke_action(Req *r, gpointer arg)
{
    int index = GPOINTER_TO_INT(arg);
    GCallback cb;

    if(!r->action_cbs || index < 0 || index >= (int)r->action_cbs->len)
        return;
    cb = g_array_index(r->action_cbs, GCallback, index);
    if(cb)
        ((PurpleRequestActionCb)cb)(r->user_data, index);
}

static int DWSIGNAL action_clicked(HWND w, void *data)
{
    ActionButton *ab = data;

    (void)w;
    complete(ab->r, invoke_action, GINT_TO_POINTER(ab->index));
    return TRUE;
}

static int DWSIGNAL action_delete(HWND w, void *data)
{
    Req *r = data;

    (void)w;
    /* Closing the window picks the last action: libpurple callers put the
     * negative choice ("Cancel", "No", "Reject") last, and running no
     * callback at all would leave e.g. certificate prompts hanging. */
    complete(r, invoke_action, GINT_TO_POINTER((int)r->action_cbs->len - 1));
    return TRUE;
}

static void *request_action_common(const char *title, const char *primary, const char *secondary,
                                   int default_action, PurpleAccount *account,
                                   gconstpointer icon_data, gsize icon_size,
                                   void *user_data, size_t action_count, va_list actions)
{
    Req *r = req_new(PURPLE_REQUEST_ACTION, user_data);
    HWND box, buttons, def = 0;
    size_t z;

    r->action_buttons = g_ptr_array_new_with_free_func(g_free);
    r->action_cbs = g_array_new(FALSE, FALSE, sizeof(GCallback));
    box = window_box(r, title);
    if(icon_data && icon_size)
    {
        HWND bmp = dw_bitmap_new(0);

        if(dw_window_set_bitmap_from_data(bmp, 0, icon_data, (int)icon_size) == DW_ERROR_NONE)
            dw_box_pack_start(box, bmp, 64, 64, FALSE, FALSE, 3);
        else
            dw_window_destroy(bmp);
    }
    add_header(box, primary, secondary, account);
    buttons = button_row(box);
    for(z = 0; z < action_count; z++)
    {
        const char *text = va_arg(actions, const char *);
        GCallback cb = va_arg(actions, GCallback);
        ActionButton *ab = g_new0(ActionButton, 1);
        char *label = dwp_strip_mnemonic(text ? text : "");
        HWND b;

        ab->r = r;
        ab->index = (int)z;
        g_ptr_array_add(r->action_buttons, ab);
        g_array_append_val(r->action_cbs, cb);
        b = dwp_button(buttons, label, DW_SIGNAL_FUNC(action_clicked), ab);
        if((int)z == default_action)
            def = b;
        g_free(label);
    }
    dw_signal_connect(r->window, DW_SIGNAL_DELETE, DW_SIGNAL_FUNC(action_delete), r);
    dwp_window_present(r->window, 480, 260);
    if(def)
        dw_window_set_focus(def);
    return r;
}

static void *request_action(const char *title, const char *primary, const char *secondary, int default_action,
                            PurpleAccount *account, const char *who, PurpleConversation *conv,
                            void *user_data, size_t action_count, va_list actions)
{
    (void)who;
    (void)conv;
    return request_action_common(title, primary, secondary, default_action, account, NULL, 0,
                                 user_data, action_count, actions);
}

static void *request_action_with_icon(const char *title, const char *primary, const char *secondary,
                                      int default_action, PurpleAccount *account, const char *who,
                                      PurpleConversation *conv, gconstpointer icon_data, gsize icon_size,
                                      void *user_data, size_t action_count, va_list actions)
{
    (void)who;
    (void)conv;
    return request_action_common(title, primary, secondary, default_action, account, icon_data, icon_size,
                                 user_data, action_count, actions);
}

/* ================================================================= fields */

static HWND make_field_widget(Req *r, FieldWidget *fw, HWND box)
{
    PurpleRequestField *field = fw->field;
    char *label = dwp_strip_mnemonic(purple_request_field_get_label(field));
    char *shown;
    HWND w = 0;

    shown = (purple_request_field_is_required(field) && *label) ? g_strdup_printf("%s *", label) : g_strdup(label);
    switch(purple_request_field_get_type(field))
    {
        case PURPLE_REQUEST_FIELD_STRING:
        {
            const char *def = purple_request_field_string_get_default_value(field);

            if(purple_request_field_string_is_multiline(field))
            {
                fw->multiline = TRUE;
                w = dw_mle_new(0);
                dw_mle_set_word_wrap(w, TRUE);
                if(def)
                    dw_mle_import(w, def, 0);
                dw_mle_set_editable(w, purple_request_field_string_is_editable(field));
                dw_box_pack_start(box, dwp_label(shown), DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 1);
                dw_box_pack_start(box, w, 380, 90, TRUE, TRUE, 2);
            }
            else
            {
                w = purple_request_field_string_is_masked(field)
                    ? dw_entryfield_password_new(def ? def : "", 0)
                    : dw_entryfield_new(def ? def : "", 0);
                if(!purple_request_field_string_is_editable(field))
                    dw_window_disable(w);
                dwp_form_row(box, shown, w);
            }
            break;
        }
        case PURPLE_REQUEST_FIELD_INTEGER:
        {
            char buf[32];

            g_snprintf(buf, sizeof(buf), "%d", purple_request_field_int_get_default_value(field));
            w = dw_entryfield_new(buf, 0);
            dwp_form_row(box, shown, w);
            break;
        }
        case PURPLE_REQUEST_FIELD_BOOLEAN:
            w = dw_checkbox_new(label, 0);
            dw_checkbox_set(w, purple_request_field_bool_get_default_value(field));
            dwp_form_row(box, "", w);
            break;
        case PURPLE_REQUEST_FIELD_CHOICE:
        {
            GList *l;

            fw->items = g_ptr_array_new_with_free_func(g_free);
            for(l = purple_request_field_choice_get_labels(field); l; l = l->next)
                g_ptr_array_add(fw->items, dwp_strip_mnemonic(l->data));
            w = dw_combobox_new("", 0);
            dwp_combo_fill(w, (char **)fw->items->pdata, (int)fw->items->len,
                           purple_request_field_choice_get_default_value(field));
            dwp_form_row(box, shown, w);
            break;
        }
        case PURPLE_REQUEST_FIELD_LIST:
        {
            GList *l;
            int z = 0;

            fw->items = g_ptr_array_new_with_free_func(g_free);
            w = dw_listbox_new(0, purple_request_field_list_get_multi_select(field));
            for(l = purple_request_field_list_get_items(field); l; l = l->next, z++)
            {
                g_ptr_array_add(fw->items, g_strdup(l->data));
                dw_listbox_append(w, l->data);
            }
            for(z = 0; z < (int)fw->items->len; z++)
                if(purple_request_field_list_is_selected(field, g_ptr_array_index(fw->items, z)))
                    dw_listbox_select(w, z, TRUE);
            if(*shown)
                dw_box_pack_start(box, dwp_label(shown), DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 1);
            dw_box_pack_start(box, w, 380, 120, TRUE, TRUE, 2);
            break;
        }
        case PURPLE_REQUEST_FIELD_LABEL:
            dwp_text_block(box, purple_request_field_get_label(field), 380, 160);
            break;
        case PURPLE_REQUEST_FIELD_IMAGE:
        {
            HWND bmp = dw_bitmap_new(0);
            const char *buf = purple_request_field_image_get_buffer(field);
            gsize size = purple_request_field_image_get_size(field);

            if(buf && size &&
               dw_window_set_bitmap_from_data(bmp, 0, buf, (int)size) == DW_ERROR_NONE)
                dw_box_pack_start(box, bmp, DW_SIZE_AUTO, DW_SIZE_AUTO, FALSE, FALSE, 2);
            else
            {
                dw_window_destroy(bmp);
                dwp_form_row(box, shown, dwp_label("[image]"));
            }
            break;
        }
        case PURPLE_REQUEST_FIELD_ACCOUNT:
        {
            gboolean show_all = purple_request_field_account_get_show_all(field);
            PurpleAccount *def = purple_request_field_account_get_default_value(field);

            fw->account = dwp_account_combo_new(!show_all,
                                                (DwpAccountFilter)purple_request_field_account_get_filter(field));
            if(def)
                dwp_account_combo_set(fw->account, def);
            w = dwp_account_combo_widget(fw->account);
            dwp_form_row(box, shown, w);
            break;
        }
        default:
            break;
    }
    g_free(shown);
    g_free(label);
    (void)r;
    return w;
}

/* Copy widget values into the fields; FALSE if a required one is empty. */
static gboolean fields_collect(Req *r)
{
    guint z;

    for(z = 0; z < r->field_widgets->len; z++)
    {
        FieldWidget *fw = g_ptr_array_index(r->field_widgets, z);
        PurpleRequestField *field = fw->field;

        switch(purple_request_field_get_type(field))
        {
            case PURPLE_REQUEST_FIELD_STRING:
            {
                char *v = fw->multiline ? dwp_mle_get_text(fw->widget) : dwp_get_text(fw->widget);

                purple_request_field_string_set_value(field, *v ? v : NULL);
                g_free(v);
                break;
            }
            case PURPLE_REQUEST_FIELD_INTEGER:
            {
                char *v = dwp_get_text_trimmed(fw->widget);

                purple_request_field_int_set_value(field, atoi(v));
                g_free(v);
                break;
            }
            case PURPLE_REQUEST_FIELD_BOOLEAN:
                purple_request_field_bool_set_value(field, dw_checkbox_get(fw->widget) ? TRUE : FALSE);
                break;
            case PURPLE_REQUEST_FIELD_CHOICE:
            {
                int index = dwp_combo_index(fw->widget, (char **)fw->items->pdata, (int)fw->items->len);

                if(index >= 0)
                    purple_request_field_choice_set_value(field, index);
                break;
            }
            case PURPLE_REQUEST_FIELD_LIST:
            {
                GList *sel = NULL;
                int where = -1;

                if(purple_request_field_list_get_multi_select(field))
                {
                    while((where = dw_listbox_selected_multi(fw->widget, where)) != -1)
                        if(where >= 0 && where < (int)fw->items->len)
                            sel = g_list_append(sel, g_ptr_array_index(fw->items, where));
                }
                else
                {
                    where = dw_listbox_selected(fw->widget);
                    if(where >= 0 && where < (int)fw->items->len)
                        sel = g_list_append(sel, g_ptr_array_index(fw->items, where));
                }
                purple_request_field_list_set_selected(field, sel);    /* copies the strings */
                g_list_free(sel);
                break;
            }
            case PURPLE_REQUEST_FIELD_ACCOUNT:
                purple_request_field_account_set_value(field, dwp_account_combo_get(fw->account));
                break;
            default:
                break;
        }
    }
    return purple_request_fields_all_required_filled(r->fields);
}

static void invoke_fields(Req *r, gpointer arg)
{
    GCallback cb = arg;

    if(cb)
        ((PurpleRequestFieldsCb)cb)(r->user_data, r->fields);
}

static int DWSIGNAL fields_ok(HWND w, void *data)
{
    Req *r = data;

    (void)w;
    if(r->done)
        return TRUE;
    if(!fields_collect(r))
    {
        dw_messagebox(DWP_NAME, DW_MB_OK | DW_MB_WARNING, "Please fill in all required fields (marked *).");
        return TRUE;
    }
    complete(r, invoke_fields, (gpointer)r->ok_cb);
    return TRUE;
}

static int DWSIGNAL fields_cancel(HWND w, void *data)
{
    Req *r = data;

    (void)w;
    if(!r->done)
    {
        fields_collect(r);
        complete(r, invoke_fields, (gpointer)r->cancel_cb);
    }
    return TRUE;
}

static void *request_fields(const char *title, const char *primary, const char *secondary,
                            PurpleRequestFields *fields, const char *ok_text, GCallback ok_cb,
                            const char *cancel_text, GCallback cancel_cb, PurpleAccount *account,
                            const char *who, PurpleConversation *conv, void *user_data)
{
    Req *r = req_new(PURPLE_REQUEST_FIELDS, user_data);
    HWND box, scroll, buttons, inner;
    GList *g, *f;
    int count = 0;
    char *okt = button_text(ok_text, "OK"), *cancelt = button_text(cancel_text, "Cancel");

    (void)who;
    (void)conv;
    r->fields = fields;
    r->ok_cb = ok_cb;
    r->cancel_cb = cancel_cb;
    r->field_widgets = g_ptr_array_new_with_free_func(field_widget_free);
    box = window_box(r, title);
    add_header(box, primary, secondary, account);
    scroll = dw_scrollbox_new(DW_VERT, 2);
    dw_box_pack_start(box, scroll, 440, 260, TRUE, TRUE, 2);
    for(g = purple_request_fields_get_groups(fields); g; g = g->next)
    {
        PurpleRequestFieldGroup *group = g->data;
        const char *gtitle = purple_request_field_group_get_title(group);

        if(gtitle && *gtitle)
        {
            inner = dw_groupbox_new(DW_VERT, 2, gtitle);
            dw_box_pack_start(scroll, inner, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 2);
        }
        else
            inner = scroll;
        for(f = purple_request_field_group_get_fields(group); f; f = f->next)
        {
            PurpleRequestField *field = f->data;
            FieldWidget *fw;

            if(!purple_request_field_is_visible(field))
                continue;
            fw = g_new0(FieldWidget, 1);
            fw->field = field;
            fw->widget = make_field_widget(r, fw, inner);
            g_ptr_array_add(r->field_widgets, fw);
            count++;
        }
    }
    buttons = button_row(box);
    dwp_button(buttons, okt, DW_SIGNAL_FUNC(fields_ok), r);
    dwp_button(buttons, cancelt, DW_SIGNAL_FUNC(fields_cancel), r);
    dw_signal_connect(r->window, DW_SIGNAL_DELETE, DW_SIGNAL_FUNC(fields_cancel), r);
    dwp_window_present(r->window, 500, MIN(160 + count * 34, 560));
    g_free(okt);
    g_free(cancelt);
    return r;
}

/* ============================================================ file/folder */

static void invoke_file(Req *r, gpointer arg)
{
    GCallback cb = arg;

    if(cb)
        ((PurpleRequestFileCb)cb)(r->user_data, r->filename);
}

static void file_task(gpointer data)
{
    Req *r = data;
    char *path;
    int flags;

    if(r->closed || r->done)
        return;
    flags = r->type == PURPLE_REQUEST_FOLDER ? DW_DIRECTORY_OPEN : (r->save ? DW_FILE_SAVE : DW_FILE_OPEN);
    r->busy = TRUE;
    path = dw_file_browse(dialog_title(r->title), r->filename, NULL, flags);
    r->busy = FALSE;
    /* libpurple may have closed the request while the dialog was open. */
    if(r->closed || r->done)
    {
        if(path)
            dw_free(path);
        return;
    }
    if(path && *path)
    {
        g_free(r->filename);
        r->filename = g_strdup(path);
        complete(r, invoke_file, (gpointer)r->ok_cb);
    }
    else
    {
        g_free(r->filename);
        r->filename = NULL;
        complete(r, invoke_file, (gpointer)r->cancel_cb);
    }
    if(path)
        dw_free(path);
}

static void *request_file(const char *title, const char *filename, gboolean savedialog,
                          GCallback ok_cb, GCallback cancel_cb, PurpleAccount *account,
                          const char *who, PurpleConversation *conv, void *user_data)
{
    Req *r = req_new(PURPLE_REQUEST_FILE, user_data);

    (void)account;
    (void)who;
    (void)conv;
    r->ok_cb = ok_cb;
    r->cancel_cb = cancel_cb;
    r->title = g_strdup(title);
    r->filename = g_strdup(filename);
    r->save = savedialog;
    /* Never block inside a libpurple call: open the dialog from the loop. */
    dwp_defer(file_task, r);
    return r;
}

static void *request_folder(const char *title, const char *dirname, GCallback ok_cb, GCallback cancel_cb,
                            PurpleAccount *account, const char *who, PurpleConversation *conv,
                            void *user_data)
{
    Req *r = req_new(PURPLE_REQUEST_FOLDER, user_data);

    (void)account;
    (void)who;
    (void)conv;
    r->ok_cb = ok_cb;
    r->cancel_cb = cancel_cb;
    r->title = g_strdup(title);
    r->filename = g_strdup(dirname);
    dwp_defer(file_task, r);
    return r;
}

static void close_request(PurpleRequestType type, void *ui_handle)
{
    Req *r = ui_handle;

    (void)type;
    if(!g_list_find(live, r))
        return;
    close_from_core(r);
}

PurpleRequestUiOps *dwp_request_ui_ops(void)
{
    static PurpleRequestUiOps ops;
    static gboolean ready = FALSE;

    if(!ready)
    {
        memset(&ops, 0, sizeof(ops));
        ops.request_input = request_input;
        ops.request_choice = request_choice;
        ops.request_action = request_action;
        ops.request_fields = request_fields;
        ops.request_file = request_file;
        ops.close_request = close_request;
        ops.request_folder = request_folder;
        ops.request_action_with_icon = request_action_with_icon;
        ready = TRUE;
    }
    return &ops;
}
