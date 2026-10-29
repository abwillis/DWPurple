/*
 * DWPurple - system idle detection (PurpleIdleUiOps).
 *
 * Sources, in order:
 *   Windows        GetLastInputInfo()
 *   X11            XScreenSaver extension (build with HAVE_XSS=1)
 *   everywhere     keyboard/mouse activity in DWPurple's own windows
 * libpurple polls get_time_idle and handles auto-away itself, using the
 * /purple/away/idle_reporting and /purple/away/mins_before_away prefs.
 */
#include "dwpurple.h"

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef HAVE_XSS
#include <X11/Xlib.h>
#include <X11/extensions/scrnsaver.h>

static Display *xss_display = NULL;
static XScreenSaverInfo *xss_info = NULL;
static gboolean xss_failed = FALSE;

static gboolean xss_query(time_t *secs)
{
    int event_base, error_base;

    if(xss_failed)
        return FALSE;
    if(!xss_display)
    {
        const char *disp = g_getenv("DISPLAY");

        /* Pure Wayland sessions have no X display to ask. */
        if(!disp || !*disp || !(xss_display = XOpenDisplay(NULL)))
        {
            xss_failed = TRUE;
            return FALSE;
        }
        if(!XScreenSaverQueryExtension(xss_display, &event_base, &error_base) ||
           !(xss_info = XScreenSaverAllocInfo()))
        {
            XCloseDisplay(xss_display);
            xss_display = NULL;
            xss_failed = TRUE;
            return FALSE;
        }
    }
    if(!XScreenSaverQueryInfo(xss_display, DefaultRootWindow(xss_display), xss_info))
        return FALSE;
    *secs = (time_t)(xss_info->idle / 1000);
    return TRUE;
}

static void xss_close(void)
{
    if(xss_info)
        XFree(xss_info);
    if(xss_display)
        XCloseDisplay(xss_display);
    xss_info = NULL;
    xss_display = NULL;
}
#endif

static const char *backend = "Activity in DWPurple windows only";

static time_t app_idle(void)
{
    time_t now = time(NULL), last = dwp_last_activity();

    return now > last ? now - last : 0;
}

static time_t get_time_idle(void)
{
#ifdef _WIN32
    LASTINPUTINFO lii;

    memset(&lii, 0, sizeof(lii));
    lii.cbSize = sizeof(lii);
    if(GetLastInputInfo(&lii))
    {
        backend = "Windows input activity";
        return (time_t)((GetTickCount() - lii.dwTime) / 1000);
    }
#endif
#ifdef HAVE_XSS
    {
        time_t secs;

        if(xss_query(&secs))
        {
            backend = "X11 screen saver extension";
            return secs;
        }
    }
#endif
    backend = "Activity in DWPurple windows only";
    return app_idle();
}

PurpleIdleUiOps *dwp_idle_ui_ops(void)
{
    static PurpleIdleUiOps ops;
    static gboolean ready = FALSE;

    if(!ready)
    {
        memset(&ops, 0, sizeof(ops));
        ops.get_time_idle = get_time_idle;
        ready = TRUE;
    }
    return &ops;
}

const char *dwp_idle_backend_name(void)
{
    get_time_idle();
    return backend;
}

void dwp_idle_shutdown(void)
{
#ifdef HAVE_XSS
    xss_close();
#endif
}
