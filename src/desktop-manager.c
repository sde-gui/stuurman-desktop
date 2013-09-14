/*
 *      Copyright 2013 Vadim Ushakov <igeekless@gmail.com>
 *      Copyright 2010 - 2012 Hong Jen Yee (PCMan) <pcman.tw@gmail.com>
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

#include "desktop-manager.h"
#include "pcmanfm.h"
#include "app-config.h"
#include "wallpaper-manager.h"

#include <glib/gi18n.h>

#include <gdk/gdkx.h>
#include <gdk/gdkkeysyms.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <math.h>

#include <cairo-xlib.h>

#include "pref.h"

#include "gseal-gtk-compat.h"

static guint wallpaper_changed;

#define MAX_SCREENS  8
#define MAX_MONITORS 32
static FmDesktop * desktop_slots[MAX_SCREENS][MAX_MONITORS];

gulong monitors_changed_handler_ids[MAX_SCREENS];

static void on_wallpaper_changed(FmConfig* cfg, gpointer user_data)
{
    guint S, M;
    for (S = 0; S < MAX_SCREENS; S++)
    {
        for (M = 0; M < MAX_MONITORS; M++)
        {
            if (desktop_slots[S][M])
                wallpaper_manager_update_background(desktop_slots[S][M], M);
        }
    }
}

static gboolean finalizing;

static int _n_screens;
static GdkDisplay * _display;

static void update_desktop_slots(void);

gboolean is_desktop_slot_managed(int S, int M)
{
    if (finalizing)
    {
        if (monitors_changed_handler_ids[S])
        {
            g_signal_handler_disconnect(gdk_display_get_screen(_display, S), monitors_changed_handler_ids[S]);
            monitors_changed_handler_ids[S] = 0;
        }
        return FALSE;
    }

    if (S >= _n_screens)
        return FALSE;

    /* FIXME: TBI
    if (app_config->managed_screen >= 0 && app_config->managed_screen != S)
        return FALSE;*/

    GdkScreen * screen = gdk_display_get_screen(_display, S);

    if (!monitors_changed_handler_ids[S])
    {
        monitors_changed_handler_ids[S] =
            g_signal_connect(screen, "monitors-changed", (GCallback) update_desktop_slots, NULL);
    }

    if (M >= gdk_screen_get_n_monitors(screen))
        return FALSE;

    return TRUE;
}

static void update_desktop_slots(void)
{
    _display = gdk_display_get_default();
    _n_screens = gdk_display_get_n_screens(_display);

    guint S, M;
    for (S = 0; S < MAX_SCREENS; S++)
    {
        for (M = 0; M < MAX_MONITORS; M++)
        {
            if (is_desktop_slot_managed(S, M))
            {
                if (!desktop_slots[S][M])
                {
                    GtkWidget* desktop = (GtkWidget *) fm_desktop_new(gdk_display_get_screen(_display, S), M);
                    desktop_slots[S][M] = (FmDesktop *) desktop;
                    gtk_widget_realize(desktop);  /* without this, setting wallpaper won't work */
                    gtk_widget_show_all(desktop);
                    gdk_window_lower(gtk_widget_get_window(desktop));
                }
            }
            else
            {
                if (desktop_slots[S][M])
                {
                    gtk_widget_destroy(GTK_WIDGET(desktop_slots[S][M]));
                    desktop_slots[S][M] = NULL;
                }
            }
        }
    }
}

void fm_desktop_manager_init()
{
    const char* desktop_path;

    if(! win_group)
        win_group = gtk_window_group_new();

    /* create the ~/Desktop folder if it doesn't exist. */
    desktop_path = g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP);
    /* FIXME: should we use a localized folder name instead? */
    g_mkdir_with_parents(desktop_path, 0700); /* ensure the existance of Desktop folder. */
    /* FIXME: should we store the desktop folder path in the annoying ~/.config/user-dirs.dirs file? */

    /* FIXME: should add a possibility to use different folders on screens */
    if(!desktop_folder)
    {
        desktop_folder = fm_folder_from_path(fm_path_get_desktop());
    }

    wallpaper_manager_init();

    update_desktop_slots();

    wallpaper_changed = g_signal_connect(app_config, "changed::wallpaper", G_CALLBACK(on_wallpaper_changed), NULL);

    pcmanfm_ref();
}

void fm_desktop_manager_finalize()
{
    finalizing = TRUE;
    update_desktop_slots();

    g_object_unref(win_group);
    win_group = NULL;

    if (desktop_folder)
    {
        g_object_unref(desktop_folder);
        desktop_folder = NULL;
    }

    g_signal_handler_disconnect(app_config, wallpaper_changed);

    wallpaper_manager_finalize();

    pcmanfm_unref();
}

FmDesktop* fm_desktop_get(guint screen, guint monitor)
{
    if (screen >= MAX_SCREENS)
        return NULL;

    if (monitor >= MAX_MONITORS)
        return NULL;

    return desktop_slots[screen][monitor];
}
