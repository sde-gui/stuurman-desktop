/*
 *      pcmanfm.c
 *
 *      Copyright 2009 - 2010 Hong Jen Yee (PCMan) <pcman.tw@gmail.com>
 *      Copyright 2012 Andriy Grytsenko (LStranger) <andrej@rep.kiev.ua>
 *
 *      This program is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation; either version 2 of the License, or
 *      (at your option) any later version.
 *
 *      This program is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *      GNU General Public License for more details.
 *
 *      You should have received a copy of the GNU General Public License
 *      along with this program; if not, write to the Free Software
 *      Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *      MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <stdio.h>
#include <glib/gi18n.h>

#include <stdlib.h>
#include <string.h>
/* socket is used to keep single instance */
#include <sys/types.h>
#include <signal.h>
#include <unistd.h> /* for getcwd */
#include <sde-utils-x11.h>

#include <libsmfm-gtk/fm-gtk.h>
#include "app-config.h"
#include "desktop.h"
#include "desktop-manager.h"
#include "pref.h"
#include "pcmanfm.h"
#include "single-inst.h"

static int signal_pipe[2] = {-1, -1};
static guint save_config_idle = 0;

static char* profile = NULL;
static gboolean show_desktop = TRUE;
static gboolean check_running = FALSE;
static gboolean desktop_off = FALSE;
static gboolean desktop_running = FALSE;
static gboolean preferences = FALSE;
static char* set_wallpaper = NULL;
static char* wallpaper_mode = NULL;
static char* ipc_cwd = NULL;
static char* window_role = NULL;

static int n_pcmanfm_ref = 0;

static GOptionEntry opt_entries[] =
{
    /* options only acceptable by first pcmanfm instance. These options are not passed through IPC */
    { "profile", 'p', 0, G_OPTION_ARG_STRING, &profile, N_("Name of configuration profile"), N_("PROFILE") },

    /* options that are acceptable for every instance of pcmanfm and will be passed through IPC. */
    /*{ "desktop", '\0', 0, G_OPTION_ARG_NONE, &show_desktop, N_("Launch desktop manager"), NULL },*/
    { "check-running", '\0', 0, G_OPTION_ARG_NONE, &check_running, N_("Check if an instance of stuurman-desktop is running. Exits with zero status if another copy of stuurman-desktop is running."), NULL },
    { "desktop-off", '\0', 0, G_OPTION_ARG_NONE, &desktop_off, N_("Turn off desktop manager if it's running"), NULL },
    { "preferences", '\0', 0, G_OPTION_ARG_NONE, &preferences, N_("Open desktop preferences dialog"), NULL },
    { "set-wallpaper", 'w', 0, G_OPTION_ARG_FILENAME, &set_wallpaper, N_("Set desktop wallpaper from image FILE"), N_("FILE") },
                    /* don't translate list of modes in description, please */
    { "wallpaper-mode", '\0', 0, G_OPTION_ARG_STRING, &wallpaper_mode, N_("Set mode of desktop wallpaper. MODE=(color|stretch|fit|center|tile)"), N_("MODE") },
    { "role", '\0', 0, G_OPTION_ARG_STRING, &window_role, N_("Window role for usage by window manager"), N_("ROLE") },
    { NULL }
};

static const char* valid_wallpaper_modes[] = {"color", "stretch", "fit", "center", "tile"};

static gboolean pcmanfm_run();

/* it's not safe to call gtk+ functions in unix signal handler
 * since the process is interrupted here and the state of gtk+ is unpredictable. */
static void unix_signal_handler(int sig_num)
{
    /* postpond the signal handling by using a pipe */
    if (write(signal_pipe[1], &sig_num, sizeof(sig_num)) != sizeof(sig_num)) {
        g_critical("cannot bounce the signal, stop");
        _exit(2);
    }
}

static gboolean on_unix_signal(GIOChannel* ch, GIOCondition cond, gpointer user_data)
{
    int sig_num;
    GIOStatus status;
    gsize got;

    while(1)
    {
        status = g_io_channel_read_chars(ch, (gchar*)&sig_num, sizeof(sig_num),
                                         &got, NULL);
        if(status == G_IO_STATUS_AGAIN) /* we read all the pipe */
        {
            g_debug("got G_IO_STATUS_AGAIN");
            return TRUE;
        }
        if(status != G_IO_STATUS_NORMAL || got != sizeof(sig_num)) /* broken pipe */
        {
            g_debug("signal pipe is broken");
            gtk_main_quit();
            return FALSE;
        }
        g_debug("got signal %d from pipe", sig_num);
        switch(sig_num)
        {
        case SIGTERM:
        default:
            gtk_main_quit();
            return FALSE;
        }
    }
    return TRUE;
}

static void single_inst_cb(const char* cwd, int screen_num)
{
    g_free(ipc_cwd);
    ipc_cwd = g_strdup(cwd);

    pcmanfm_run();
    window_role = NULL; /* reset it for clients callbacks */
}

int main(int argc, char** argv)
{
    FmConfig* config;
    GError* err = NULL;
    SingleInstData inst;

#ifdef ENABLE_NLS
    bindtextdomain ( GETTEXT_PACKAGE, PACKAGE_LOCALE_DIR );
    bind_textdomain_codeset ( GETTEXT_PACKAGE, "UTF-8" );
    textdomain ( GETTEXT_PACKAGE );
#endif

    /* initialize GTK+ and parse the command line arguments */
    if(G_UNLIKELY(!gtk_init_with_args(&argc, &argv, " ", opt_entries, GETTEXT_PACKAGE, &err)))
    {
        g_printf("%s\n", err->message);
        g_error_free(err);
        return 1;
    }

    su_x11_resolve_well_known_atoms(gdk_x11_get_default_xdisplay());

    /* ensure that there is only one instance of pcmanfm-desktop. */
    inst.prog_name = "stuurman-desktop";
    inst.cb = single_inst_cb;
    inst.opt_entries = opt_entries;
    inst.screen_num = gdk_x11_get_default_screen();
    switch(single_inst_init(&inst))
    {
    case SINGLE_INST_CLIENT: /* we're not the first instance. */
        single_inst_finalize(&inst);
        gdk_notify_startup_complete();
        return 0;
    case SINGLE_INST_ERROR: /* error happened. */
        single_inst_finalize(&inst);
        return 1;
    case SINGLE_INST_SERVER: ; /* FIXME */
    }

    if (check_running)
        return 1;

    if(pipe(signal_pipe) == 0)
    {
        GIOChannel* ch = g_io_channel_unix_new(signal_pipe[0]);
        g_io_add_watch(ch, G_IO_IN|G_IO_PRI, (GIOFunc)on_unix_signal, NULL);
        g_io_channel_set_encoding(ch, NULL, NULL);
        g_io_channel_unref(ch);

        /* intercept signals */
        // signal( SIGPIPE, SIG_IGN );
        signal( SIGHUP, unix_signal_handler );
        signal( SIGTERM, unix_signal_handler );
        signal( SIGINT, unix_signal_handler );
    }

    config = fm_app_config_new(); /* this automatically load libfm config file. */

    /* load pcmanfm-specific config file */
    fm_app_config_load_from_profile(FM_APP_CONFIG(config), profile);

    fm_gtk_init(config);
    /* the main part */
    if(pcmanfm_run(gdk_screen_get_number(gdk_screen_get_default())))
    {
        window_role = NULL; /* reset it for clients callbacks */
        gtk_main();
        /* g_debug("main loop ended"); */
        if(desktop_running)
            fm_desktop_manager_finalize();

        pcmanfm_save_config(TRUE);
        if(save_config_idle)
        {
            g_source_remove(save_config_idle);
            save_config_idle = 0;
        }
    }

    single_inst_finalize(&inst);
    fm_gtk_finalize();

    g_object_unref(config);
    return 0;
}

gboolean pcmanfm_run()
{
    gboolean ret = TRUE;

    if (check_running)
    {
        /* Do nothing. */
        check_running = FALSE;
        return TRUE;
    }

    if (preferences)
    {
        /* FIXME: pass screen number from client */
        fm_desktop_preference(NULL, GTK_WINDOW(fm_desktop_get(0, 0)));
        preferences = FALSE;
        return TRUE;
    }

    if(show_desktop)
    {
        if(!desktop_running)
        {
            fm_desktop_manager_init();
            desktop_running = TRUE;
        }
        show_desktop = FALSE;
        return TRUE;
    }
    else if(desktop_off)
    {
        if(desktop_running)
        {
            desktop_running = FALSE;
            fm_desktop_manager_finalize();
        }
        desktop_off = FALSE;
        return FALSE;
    }
    else
    {
        gboolean need_to_exit = (wallpaper_mode || set_wallpaper);
        gboolean wallpaper_changed = FALSE;
        if(set_wallpaper) /* a new wallpaper is assigned */
        {
            /* g_debug("\'%s\'", set_wallpaper); */
            /* Make sure this is a support image file. */
            if(gdk_pixbuf_get_file_info(set_wallpaper, NULL, NULL))
            {
                if(app_config->wallpaper)
                    g_free(app_config->wallpaper);
                app_config->wallpaper = set_wallpaper;
                if(! wallpaper_mode) /* if wallpaper mode is not specified */
                {
                    /* do not use solid color mode; otherwise wallpaper won't be shown. */
                    if(app_config->wallpaper_mode == FM_WP_COLOR)
                        app_config->wallpaper_mode = FM_WP_FIT;
                }
                wallpaper_changed = TRUE;
            }
            else
                g_free(set_wallpaper);
            set_wallpaper = NULL;
        }

        if(wallpaper_mode)
        {
            guint i = 0;
            for(i = 0; i < G_N_ELEMENTS(valid_wallpaper_modes); ++i)
            {
                if(strcmp(valid_wallpaper_modes[i], wallpaper_mode) == 0)
                {
                    if(i != app_config->wallpaper_mode)
                    {
                        app_config->wallpaper_mode = i;
                        wallpaper_changed = TRUE;
                    }
                    break;
                }
            }
            g_free(wallpaper_mode);
            wallpaper_mode = NULL;
        }

        if(wallpaper_changed)
        {
            fm_config_emit_changed(FM_CONFIG(app_config), "wallpaper");
            fm_app_config_save_profile(app_config, profile);
        }

        if(need_to_exit)
            return FALSE;
    }

    return ret;
}

/* After opening any window/dialog/tool, this should be called. */
void pcmanfm_ref()
{
    ++n_pcmanfm_ref;
    /* g_debug("ref: %d", n_pcmanfm_ref); */
}

/* After closing any window/dialog/tool, this should be called.
 * If the last window is closed and we are not a deamon, pcmanfm will quit.
 */
void pcmanfm_unref()
{
    --n_pcmanfm_ref;
    /* g_debug("unref: %d, daemon_mode=%d, desktop_running=%d", n_pcmanfm_ref, daemon_mode, desktop_running); */
    if( 0 == n_pcmanfm_ref && !desktop_running )
        gtk_main_quit();
}

static gboolean on_save_config_idle(gpointer user_data)
{
    pcmanfm_save_config(TRUE);
    save_config_idle = 0;
    return FALSE;
}

void pcmanfm_save_config(gboolean immediate)
{
    if(immediate)
    {
        fm_config_save(fm_config, NULL);
        fm_app_config_save_profile(app_config, profile);
    }
    else
    {
        /* install an idle handler to save the config file. */
        if( 0 == save_config_idle)
            save_config_idle = g_idle_add_full(G_PRIORITY_LOW, (GSourceFunc)on_save_config_idle, NULL, NULL);
    }
}

char* pcmanfm_get_profile_dir(gboolean create)
{
    char* dir = g_build_filename(g_get_user_config_dir(), config_app_name(), profile ? profile : "default", NULL);
    if(create)
        g_mkdir_with_parents(dir, 0700);
    return dir;
}

const char * config_app_name(void)
{
    return "stuurman-desktop";
}
