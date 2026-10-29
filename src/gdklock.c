/*
 * DWPurple - GDK 3 thread lock watchdog (GTK builds only).
 *
 * The GTK 3 port of Dynamic Windows calls gdk_threads_init(), so GDK
 * guards its event and timer dispatch with a non-recursive global lock.
 * A hang was observed with the main thread blocked inside a GDK dispatch
 * waiting for that lock while no other thread was doing anything, i.e.
 * the main thread itself already held it.
 *
 * This replaces GDK's lock functions (before dw_init() calls
 * gdk_threads_init()) with an owner-tracking version that:
 *   - lets the owning thread re-enter instead of deadlocking;
 *   - ignores a release by a thread that does not hold the lock instead
 *     of aborting;
 *   - prints both stacks to stderr when either happens: where the lock was
 *     taken and where it was taken again, so the unbalanced call can be
 *     found.
 *
 * Set DWP_GDK_DEFAULT_LOCK=1 to keep GDK's own lock.  On other platforms
 * (or Dynamic Windows ports without GDK 3) this does nothing.
 */
#if defined(__linux__) || defined(__FreeBSD__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif

#include "dwpurple.h"

#include <stdio.h>

#if defined(__GLIBC__) && !defined(_WIN32) && !defined(__OS2__) && !defined(__APPLE__)
#include <dlfcn.h>
#include <execinfo.h>

#define STACK_DEPTH 48
#define MAX_REPORTS 8

static GMutex state;
static GCond released;
static GThread *owner = NULL;
static int depth = 0;
static void *owner_stack[STACK_DEPTH];
static int owner_frames = 0;
static int reports = 0;

/* Caller holds "state". */
static void report(const char *what)
{
    void *here[STACK_DEPTH];
    int n;

    if(reports >= MAX_REPORTS)
        return;
    reports++;
    fprintf(stderr, "\n*** dwpurple gdk-lock: %s\n*** current stack:\n", what);
    n = backtrace(here, STACK_DEPTH);
    backtrace_symbols_fd(here, n, 2);
    if(owner_frames)
    {
        fprintf(stderr, "*** the lock was taken here:\n");
        backtrace_symbols_fd(owner_stack, owner_frames, 2);
    }
    fflush(stderr);
}

static void lock_enter(void)
{
    GThread *self = g_thread_self();

    g_mutex_lock(&state);
    if(owner == self)
    {
        depth++;
        report("taken again by the thread that already holds it (GDK would deadlock here)");
        g_mutex_unlock(&state);
        return;
    }
    while(owner)
    {
        gint64 until = g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND;

        if(!g_cond_wait_until(&released, &state, until) && owner)
            report("waited 3 seconds for the lock held by another thread");
    }
    owner = self;
    depth = 1;
    owner_frames = backtrace(owner_stack, STACK_DEPTH);
    g_mutex_unlock(&state);
}

static void lock_leave(void)
{
    g_mutex_lock(&state);
    if(owner != g_thread_self())
    {
        report("released by a thread that does not hold it (ignored; GLib would abort)");
        g_mutex_unlock(&state);
        return;
    }
    if(--depth == 0)
    {
        owner = NULL;
        owner_frames = 0;
        g_cond_signal(&released);
    }
    g_mutex_unlock(&state);
}

void dwp_gdk_lock_install(void)
{
    void (*set_functions)(GCallback, GCallback);
    void *warm[2];

    if(g_getenv("DWP_GDK_DEFAULT_LOCK"))
        return;
    *(void **)(&set_functions) = dlsym(RTLD_DEFAULT, "gdk_threads_set_lock_functions");
    if(!set_functions)
        return;
    /* The first backtrace() loads the unwinder; do it outside the lock. */
    backtrace(warm, 2);
    set_functions(G_CALLBACK(lock_enter), G_CALLBACK(lock_leave));
}

static void (*gui_enter)(void) = NULL;
static void (*gui_leave)(void) = NULL;
static gboolean gui_resolved = FALSE;

static void gui_resolve(void)
{
    if(gui_resolved)
        return;
    gui_resolved = TRUE;
    *(void **)(&gui_enter) = dlsym(RTLD_DEFAULT, "gdk_threads_enter");
    *(void **)(&gui_leave) = dlsym(RTLD_DEFAULT, "gdk_threads_leave");
    if(!gui_enter || !gui_leave)
        gui_enter = gui_leave = NULL;
}

/* Dynamic Windows timers are plain GLib timeouts, so their callbacks run
 * without the GDK lock, while GTK 3's gtk_events_pending()/
 * gtk_main_iteration() (used by Dynamic Windows' nested loops, for example
 * when sizing a window) release and retake it.  Unheld, that leaves the
 * lock owned by the main thread when the callback returns, and the next GDK
 * dispatch deadlocks. */
void dwp_gui_lock(void)
{
    gui_resolve();
    if(gui_enter)
        gui_enter();
}

void dwp_gui_unlock(void)
{
    if(gui_leave)
        gui_leave();
}

#else

void dwp_gdk_lock_install(void)
{
}

void dwp_gui_lock(void)
{
}

void dwp_gui_unlock(void)
{
}

#endif
