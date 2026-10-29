/*
 * DWPurple - libpurple event loop on top of Dynamic Windows.
 *
 * Dynamic Windows has timers but no file descriptor watches, so a single
 * periodic "pump" timer services both:
 *   - libpurple timeouts, kept in our own table with monotonic deadlines;
 *   - libpurple input watches, polled with a zero-timeout select().
 * This works the same on OS/2 (kLIBC select handles sockets and pipes),
 * Windows (sockets) and Unix.
 *
 * Re-entrancy: a callback may start a nested message loop (OS/2 runs the
 * pump timer inside it), so each source carries a "running" flag and is
 * looked up again by id after every callback.
 */
#include "dwpurple.h"

#include <errno.h>
#ifndef _WIN32
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>
#ifdef __OS2__
#include <sys/socket.h>
#endif
#include <sys/select.h>
#endif

#define PUMP_INTERVAL_MS 20

typedef struct {
    guint id;
    guint interval;
    gint64 due;          /* milliseconds, monotonic */
    GSourceFunc func;
    gpointer data;
    gboolean running;
} Timeout;

typedef struct {
    guint id;
    int fd;
    PurpleInputCondition cond;
    PurpleInputFunction func;
    gpointer data;
    gboolean running;
} Input;

static GMutex lock;
static GHashTable *timeouts = NULL;   /* id -> Timeout */
static GHashTable *inputs = NULL;     /* id -> Input */
static guint next_id = 1;
static HTIMER pump_timer = 0;
static gboolean shutting_down = FALSE;

static gint64 now_ms(void)
{
    return g_get_monotonic_time() / 1000;
}

static void ensure_tables(void)
{
    if(!timeouts)
    {
        timeouts = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
        inputs = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    }
}

/* Caller holds the lock. */
static guint new_id(void)
{
    guint id;

    do
    {
        id = next_id++;
        if(next_id == 0)
            next_id = 1;
    } while(id == 0 ||
            g_hash_table_lookup(timeouts, GUINT_TO_POINTER(id)) ||
            g_hash_table_lookup(inputs, GUINT_TO_POINTER(id)));
    return id;
}

static guint add_timeout_ms(guint interval, GSourceFunc func, gpointer data)
{
    Timeout *t;
    guint id;

    g_mutex_lock(&lock);
    ensure_tables();
    t = g_new0(Timeout, 1);
    t->id = id = new_id();
    t->interval = interval;
    t->due = now_ms() + interval;
    t->func = func;
    t->data = data;
    g_hash_table_insert(timeouts, GUINT_TO_POINTER(id), t);
    g_mutex_unlock(&lock);
    return id;
}

static guint ui_timeout_add(guint interval, GSourceFunc function, gpointer data)
{
    return add_timeout_ms(interval, function, data);
}

static guint ui_timeout_add_seconds(guint interval, GSourceFunc function, gpointer data)
{
    return add_timeout_ms(interval * 1000, function, data);
}

static gboolean ui_timeout_remove(guint handle)
{
    gboolean ret;

    g_mutex_lock(&lock);
    ensure_tables();
    ret = g_hash_table_remove(timeouts, GUINT_TO_POINTER(handle));
    g_mutex_unlock(&lock);
    return ret;
}

static guint ui_input_add(int fd, PurpleInputCondition cond, PurpleInputFunction func, gpointer data)
{
    Input *in;
    guint id;

    if(fd < 0)
    {
        purple_debug_warning("eventloop", "input_add called with invalid fd %d\n", fd);
        return 0;
    }
#ifndef _WIN32
    if(fd >= FD_SETSIZE)
    {
        purple_debug_error("eventloop", "fd %d exceeds FD_SETSIZE; not watched\n", fd);
        return 0;
    }
#endif
    g_mutex_lock(&lock);
    ensure_tables();
    in = g_new0(Input, 1);
    in->id = id = new_id();
    in->fd = fd;
    in->cond = cond;
    in->func = func;
    in->data = data;
    g_hash_table_insert(inputs, GUINT_TO_POINTER(id), in);
    g_mutex_unlock(&lock);
    return id;
}

static gboolean ui_input_remove(guint handle)
{
    gboolean ret;

    g_mutex_lock(&lock);
    ensure_tables();
    ret = g_hash_table_remove(inputs, GUINT_TO_POINTER(handle));
    g_mutex_unlock(&lock);
    return ret;
}

/* ------------------------------------------------------------------ pump */

static void run_timeouts(void)
{
    GArray *due;
    GHashTableIter iter;
    gpointer key, value;
    gint64 now = now_ms();
    guint z;

    due = g_array_new(FALSE, FALSE, sizeof(guint));
    g_mutex_lock(&lock);
    g_hash_table_iter_init(&iter, timeouts);
    while(g_hash_table_iter_next(&iter, &key, &value))
    {
        Timeout *t = value;

        if(!t->running && t->due <= now)
            g_array_append_val(due, t->id);
    }
    g_mutex_unlock(&lock);

    for(z = 0; z < due->len; z++)
    {
        guint id = g_array_index(due, guint, z);
        GSourceFunc func;
        gpointer data;
        gboolean again;
        Timeout *t;

        g_mutex_lock(&lock);
        t = g_hash_table_lookup(timeouts, GUINT_TO_POINTER(id));
        if(!t || t->running)
        {
            g_mutex_unlock(&lock);
            continue;
        }
        t->running = TRUE;
        t->due = now_ms() + t->interval;
        func = t->func;
        data = t->data;
        g_mutex_unlock(&lock);

        again = func(data);

        g_mutex_lock(&lock);
        t = g_hash_table_lookup(timeouts, GUINT_TO_POINTER(id));
        if(t)
        {
            t->running = FALSE;
            if(!again)
                g_hash_table_remove(timeouts, GUINT_TO_POINTER(id));
        }
        g_mutex_unlock(&lock);
    }
    g_array_free(due, TRUE);
}

typedef struct {
    guint id;
    int fd;
    PurpleInputCondition cond;
} Watch;

static PurpleInputCondition poll_one(int fd, PurpleInputCondition want, gboolean *bad)
{
    fd_set rfds, wfds;
    struct timeval tv = { 0, 0 };
    PurpleInputCondition got = 0;
    int rc;

    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    if(want & PURPLE_INPUT_READ)
        FD_SET(fd, &rfds);
    if(want & PURPLE_INPUT_WRITE)
        FD_SET(fd, &wfds);
    rc = select(fd + 1, &rfds, &wfds, NULL, &tv);
    if(rc < 0)
    {
        *bad = TRUE;
        return 0;
    }
    if(rc > 0)
    {
        if(FD_ISSET(fd, &rfds))
            got |= PURPLE_INPUT_READ;
        if(FD_ISSET(fd, &wfds))
            got |= PURPLE_INPUT_WRITE;
    }
    return got;
}

static void run_inputs(void)
{
    GArray *watches;
    GHashTableIter iter;
    gpointer key, value;
    fd_set rfds, wfds;
    struct timeval tv = { 0, 0 };
    int maxfd = -1, rc;
    guint z;
    gboolean fallback = FALSE;

    watches = g_array_new(FALSE, FALSE, sizeof(Watch));
    g_mutex_lock(&lock);
    g_hash_table_iter_init(&iter, inputs);
    while(g_hash_table_iter_next(&iter, &key, &value))
    {
        Input *in = value;
        Watch w;

        if(in->running)
            continue;
        w.id = in->id;
        w.fd = in->fd;
        w.cond = in->cond;
        g_array_append_val(watches, w);
    }
    g_mutex_unlock(&lock);

    if(!watches->len)
    {
        g_array_free(watches, TRUE);
        return;
    }

    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    for(z = 0; z < watches->len; z++)
    {
        Watch *w = &g_array_index(watches, Watch, z);

        if(w->cond & PURPLE_INPUT_READ)
            FD_SET(w->fd, &rfds);
        if(w->cond & PURPLE_INPUT_WRITE)
            FD_SET(w->fd, &wfds);
        if(w->fd > maxfd)
            maxfd = w->fd;
    }
    rc = select(maxfd + 1, &rfds, &wfds, NULL, &tv);
    if(rc < 0)
    {
        /* Usually EBADF: a protocol closed a descriptor before removing its
         * watch.  Poll each descriptor separately and skip the bad ones. */
        if(errno == EINTR)
        {
            g_array_free(watches, TRUE);
            return;
        }
        fallback = TRUE;
    }
    else if(rc == 0)
    {
        g_array_free(watches, TRUE);
        return;
    }

    for(z = 0; z < watches->len; z++)
    {
        Watch *w = &g_array_index(watches, Watch, z);
        PurpleInputCondition got = 0;
        PurpleInputFunction func;
        gpointer data;
        Input *in;

        if(fallback)
        {
            gboolean bad = FALSE;

            got = poll_one(w->fd, w->cond, &bad);
            if(bad)
                continue;
        }
        else
        {
            if((w->cond & PURPLE_INPUT_READ) && FD_ISSET(w->fd, &rfds))
                got |= PURPLE_INPUT_READ;
            if((w->cond & PURPLE_INPUT_WRITE) && FD_ISSET(w->fd, &wfds))
                got |= PURPLE_INPUT_WRITE;
        }
        if(!got)
            continue;

        g_mutex_lock(&lock);
        in = g_hash_table_lookup(inputs, GUINT_TO_POINTER(w->id));
        if(!in || in->running || in->fd != w->fd)
        {
            g_mutex_unlock(&lock);
            continue;
        }
        in->running = TRUE;
        func = in->func;
        data = in->data;
        g_mutex_unlock(&lock);

        func(data, w->fd, got);

        g_mutex_lock(&lock);
        in = g_hash_table_lookup(inputs, GUINT_TO_POINTER(w->id));
        if(in)
            in->running = FALSE;
        g_mutex_unlock(&lock);
    }
    g_array_free(watches, TRUE);
}

static int DWSIGNAL pump_cb(void *unused)
{
    (void)unused;
    if(shutting_down || !timeouts)
        return TRUE;
    /* libpurple callbacks open windows; GTK needs the GDK lock held. */
    dwp_gui_lock();
    run_inputs();
    run_timeouts();
    dwp_gui_unlock();
    return TRUE;
}

void dwp_eventloop_start(void)
{
    g_mutex_lock(&lock);
    ensure_tables();
    g_mutex_unlock(&lock);
    if(!pump_timer)
        pump_timer = dw_timer_connect(PUMP_INTERVAL_MS, DW_SIGNAL_FUNC(pump_cb), NULL);
}

void dwp_eventloop_shutdown(void)
{
    shutting_down = TRUE;
    if(pump_timer)
    {
        dw_timer_disconnect(pump_timer);
        pump_timer = 0;
    }
    g_mutex_lock(&lock);
    if(timeouts)
    {
        g_hash_table_destroy(timeouts);
        g_hash_table_destroy(inputs);
        timeouts = inputs = NULL;
    }
    g_mutex_unlock(&lock);
}

PurpleEventLoopUiOps *dwp_eventloop_ui_ops(void)
{
    static PurpleEventLoopUiOps ops;
    static gboolean ready = FALSE;

    if(!ready)
    {
        memset(&ops, 0, sizeof(ops));
        ops.timeout_add = ui_timeout_add;
        ops.timeout_remove = ui_timeout_remove;
        ops.input_add = ui_input_add;
        ops.input_remove = ui_input_remove;
        ops.input_get_error = NULL;       /* libpurple falls back to getsockopt */
        ops.timeout_add_seconds = ui_timeout_add_seconds;
        ready = TRUE;
    }
    return &ops;
}
