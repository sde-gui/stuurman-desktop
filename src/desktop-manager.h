/*
 *      Copyright 2013 Vadim Ushakov <igeekless@gmail.com>
 *      Copyright 2010 - 2012 Hong Jen Yee (PCMan) <pcman.tw@gmail.com>
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


#ifndef __DESKTOP_MANAGER_H__
#define __DESKTOP_MANAGER_H__

#include <gtk/gtk.h>
#include <libsmfm-gtk/fm-gtk.h>

#include "desktop.h"

G_BEGIN_DECLS

FmDesktop*  fm_desktop_get          (guint screen, guint monitor);

void fm_desktop_manager_init(gint on_screen);
void fm_desktop_manager_finalize();



GtkWindowGroup* win_group;
guint wallpaper_changed;
guint desktop_font_changed;
guint icon_theme_changed;
GtkAccelGroup* acc_grp;

PangoFontDescription* font_desc;

FmFolder* desktop_folder;


void unload_items(FmDesktop* desktop);
void load_items(FmDesktop* desktop);
void save_item_pos(FmDesktop* desktop);
void reload_icons();


G_END_DECLS

#endif /* __DESKTOP_H__ */
