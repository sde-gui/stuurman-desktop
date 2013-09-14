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

static FmDesktop **desktops;
static guint n_screens;

static void on_wallpaper_changed(FmConfig* cfg, gpointer user_data)
{
    guint i;
    for(i=0; i < n_screens; ++i)
        if(desktops[i]->monitor >= 0)
            wallpaper_manager_update_background(desktops[i], i);
}

void reload_icons()
{
    guint i;
    for(i=0; i < n_screens; ++i)
        if(desktops[i]->monitor >= 0)
            gtk_widget_queue_resize(GTK_WIDGET(desktops[i]));
}

static void on_icon_theme_changed(GtkIconTheme* theme, gpointer user_data)
{
    reload_icons();
}

void fm_desktop_manager_init(gint on_screen)
{
    GdkDisplay * gdpy;
    guint i, n_scr, n_mon, scr, mon;
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

    if(app_config->desktop_font)
        font_desc = pango_font_description_from_string(app_config->desktop_font);

    wallpaper_manager_init();

    gdpy = gdk_display_get_default();
    n_scr = gdk_display_get_n_screens(gdpy);
    n_screens = 0;
    for(i = 0; i < n_scr; i++)
        n_screens += gdk_screen_get_n_monitors(gdk_display_get_screen(gdpy, i));
    desktops = g_new(FmDesktop*, n_screens);
    for(scr = 0, i = 0; scr < n_scr; scr++)
    {
        GdkScreen* screen = gdk_display_get_screen(gdpy, scr);
        n_mon = gdk_screen_get_n_monitors(screen);
        for(mon = 0; mon < n_mon; mon++)
        {
            gint mon_init = (on_screen < 0 || on_screen == (int)scr) ? (int)mon : (mon ? -2 : -1);
            GtkWidget* desktop = (GtkWidget*)fm_desktop_new(screen, mon_init);
            desktops[i++] = (FmDesktop*)desktop;
            if(mon_init < 0)
                continue;
            gtk_widget_realize(desktop);  /* without this, setting wallpaper won't work */
            gtk_widget_show_all(desktop);
            gdk_window_lower(gtk_widget_get_window(desktop));
        }
    }

    wallpaper_changed = g_signal_connect(app_config, "changed::wallpaper", G_CALLBACK(on_wallpaper_changed), NULL);
    icon_theme_changed = g_signal_connect(gtk_icon_theme_get_default(), "changed", G_CALLBACK(on_icon_theme_changed), NULL);

    pcmanfm_ref();
}

void fm_desktop_manager_finalize()
{
    guint i;
    for(i = 0; i < n_screens; i++)
    {
        if(desktops[i]->monitor >= 0)
            save_item_pos(desktops[i]);
        gtk_widget_destroy(GTK_WIDGET(desktops[i]));
    }
    g_free(desktops);
    g_object_unref(win_group);
    win_group = NULL;

    if(desktop_folder)
    {
        g_object_unref(desktop_folder);
        desktop_folder = NULL;
    }

    if(font_desc)
    {
        pango_font_description_free(font_desc);
        font_desc = NULL;
    }

    g_signal_handler_disconnect(app_config, wallpaper_changed);
    g_signal_handler_disconnect(gtk_icon_theme_get_default(), icon_theme_changed);

    if(acc_grp)
        g_object_unref(acc_grp);
    acc_grp = NULL;

    wallpaper_manager_finalize();

    pcmanfm_unref();
}

FmDesktop* fm_desktop_get(guint screen, guint monitor)
{
    guint i = 0, n = 0;
    while(i < n_screens && n <= screen)
    {
        if(n == screen && desktops[i]->monitor == (gint)monitor)
            return desktops[i];
        i++;
        if(i < n_screens &&
           (desktops[i]->monitor == 0 || desktops[i]->monitor == -1))
            n++;
    }
    return NULL;
}
