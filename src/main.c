/*
 * DWPurple - entry point.
 *
 *   dwpurple [-c DIR] [-d] [-n] [-h] [-v]
 */
#include "dwpurple.h"

#include <stdio.h>
#ifndef _WIN32
#include <signal.h>
#endif

static void usage(const char *name)
{
    printf("%s %s - instant messenger using libpurple and Dynamic Windows\n\n"
           "Usage: %s [options]\n\n"
           "  -c, --config DIR   use DIR for configuration files (default ~/.dwpurple)\n"
           "  -d, --debug        print libpurple debug messages to the terminal\n"
           "  -n, --nologin      start with all accounts offline\n"
           "  -h, --help         show this help\n"
           "  -v, --version      show the version\n",
           DWP_NAME, DWP_VERSION, name);
}

int dwmain(int argc, char *argv[])
{
    DwpOptions options;
    char *error = NULL;
    int z;

    memset(&options, 0, sizeof(options));
    for(z = 1; z < argc; z++)
    {
        const char *a = argv[z];

        if((!strcmp(a, "-c") || !strcmp(a, "--config")) && z + 1 < argc)
            options.config_dir = argv[++z];
        else if(!strncmp(a, "--config=", 9))
            options.config_dir = (char *)a + 9;
        else if(!strcmp(a, "-d") || !strcmp(a, "--debug"))
            options.debug = TRUE;
        else if(!strcmp(a, "-n") || !strcmp(a, "--nologin"))
            options.no_login = TRUE;
        else if(!strcmp(a, "-v") || !strcmp(a, "--version"))
        {
            printf("%s %s\n", DWP_NAME, DWP_VERSION);
            return 0;
        }
        else if(!strcmp(a, "-h") || !strcmp(a, "--help"))
        {
            usage(argv[0]);
            return 0;
        }
        else
        {
            fprintf(stderr, "Unknown option: %s\n\n", a);
            usage(argv[0]);
            return 1;
        }
    }

#ifndef _WIN32
    /* A peer closing a socket must not kill the client. */
    signal(SIGPIPE, SIG_IGN);
#endif

    dwp_set_exe_path(argc > 0 ? argv[0] : NULL);
    /* Must run before dw_init() calls gdk_threads_init(). */
    dwp_gdk_lock_install();
    dw_app_id_set(DWP_APP_ID, DWP_NAME);
    if(dw_init(TRUE, argc, argv) != DW_ERROR_NONE)
    {
        fprintf(stderr, "Could not initialize Dynamic Windows.\n");
        return 1;
    }

    if(!dwp_core_init(&options, &error))
    {
        dw_messagebox(DWP_NAME, DW_MB_OK | DW_MB_ERROR, "%s", error ? error : "Initialization failed.");
        g_free(error);
        dw_exit(1);
        return 1;
    }

    dw_main();

    dwp_core_shutdown();
    dw_exit(0);
    return 0;
}
