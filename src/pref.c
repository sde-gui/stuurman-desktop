/*
 *      pref.c
 *
 *      Copyright 2009 PCMan <pcman.tw@gmail.com>
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
#  include <config.h>
#endif

#include <libsmfm-core/fm.h>

#include "pcmanfm.h"

#include "pref.h"
#include "app-config.h"
#include "desktop.h"

#define INIT_BOOL(b, st, name, changed_notify)  init_bool(b, #name, G_STRUCT_OFFSET(st, name), changed_notify)
#define INIT_COMBO(b, st, name, changed_notify) init_combo(b, #name, G_STRUCT_OFFSET(st, name), changed_notify)
#define INIT_INT(b, st, name, changed_notify) init_int(b, #name, G_STRUCT_OFFSET(st, name), changed_notify)
#define INIT_ICON_SIZES(b, name) init_icon_sizes(b, #name, G_STRUCT_OFFSET(FmConfig, name))
#define INIT_COLOR(b, st, name, changed_notify)  init_color(b, #name, G_STRUCT_OFFSET(st, name), changed_notify)
#define INIT_SPIN(b, st, name, changed_notify)  init_spin(b, #name, G_STRUCT_OFFSET(st, name), changed_notify)
#define INIT_ENTRY(b, st, name, changed_notify)  init_entry(b, #name, G_STRUCT_OFFSET(st, name), changed_notify)

static GtkWindow* desktop_pref_dlg = NULL;

static void on_response(GtkDialog* dlg, int res, GtkWindow** pdlg)
{
    *pdlg = NULL;
    pcmanfm_save_config(TRUE);
    gtk_widget_destroy(GTK_WIDGET(dlg));
}

static void on_combo_changed(GtkComboBox* combo, gpointer _off)
{
    gsize off = GPOINTER_TO_SIZE(_off);
    int* val = (int*)G_STRUCT_MEMBER_P(fm_config, off);
    int sel = gtk_combo_box_get_active(combo);
    if(sel != *val)
    {
        const char* name = g_object_get_data((GObject*)combo, "changed");
        if (!name)
            name = gtk_buildable_get_name((GtkBuildable*)combo);
        *val = sel;
        fm_config_emit_changed(fm_config, name);
    }
}

static void init_combo(GtkBuilder* builder, const char* name, gsize off, const char* changed_notify)
{
    GtkComboBox * combo = GTK_COMBO_BOX(gtk_builder_get_object(builder, name));
    int* val = (int*)G_STRUCT_MEMBER_P(fm_config, off);
    if (changed_notify)
        g_object_set_data_full(G_OBJECT(combo), "changed", g_strdup(changed_notify), g_free);
    gtk_combo_box_set_active(combo, *val);
    g_signal_connect(combo, "changed", G_CALLBACK(on_combo_changed), GSIZE_TO_POINTER(off));
}

static void on_spin_button_value_changed(GtkSpinButton * spin, gpointer _off)
{
    gsize off = GPOINTER_TO_SIZE(_off);
    int* val = (int*)G_STRUCT_MEMBER_P(fm_config, off);
    int sel = gtk_spin_button_get_value(spin);
    if(sel != *val)
    {
        const char* name = g_object_get_data((GObject*)spin, "changed");
        if (!name)
            name = gtk_buildable_get_name((GtkBuildable*)spin);
        *val = sel;
        fm_config_emit_changed(fm_config, name);
    }
}

static void init_int(GtkBuilder* builder, const char* name, gsize off, const char* changed_notify)
{
    GtkSpinButton * spin = GTK_SPIN_BUTTON(gtk_builder_get_object(builder, name));
    int* val = (int*)G_STRUCT_MEMBER_P(fm_config, off);
    if (changed_notify)
        g_object_set_data_full(G_OBJECT(spin), "changed", g_strdup(changed_notify), g_free);
    gtk_spin_button_set_value(spin, *val);
    g_signal_connect(spin, "value-changed", G_CALLBACK(on_spin_button_value_changed), GSIZE_TO_POINTER(off));
}


static void on_toggled(GtkToggleButton* btn, gpointer _off)
{
    gsize off = GPOINTER_TO_SIZE(_off);
    gboolean* val = (gboolean*)G_STRUCT_MEMBER_P(fm_config, off);
    gboolean new_val = gtk_toggle_button_get_active(btn);
    if(*val != new_val)
    {
        const char* name = g_object_get_data((GObject*)btn, "changed");
        if(!name)
            name = gtk_buildable_get_name((GtkBuildable*)btn);
        *val = new_val;
        fm_config_emit_changed(fm_config, name);
    }
}

static void init_bool(GtkBuilder* b, const char* name, gsize off, const char* changed_notify)
{
    GtkToggleButton* btn = GTK_TOGGLE_BUTTON(gtk_builder_get_object(b, name));
    gboolean* val = (gboolean*)G_STRUCT_MEMBER_P(fm_config, off);
    if(changed_notify)
        g_object_set_data_full(G_OBJECT(btn), "changed", g_strdup(changed_notify), g_free);
    gtk_toggle_button_set_active(btn, *val);
    g_signal_connect(btn, "toggled", G_CALLBACK(on_toggled), GSIZE_TO_POINTER(off));
}

static void on_color_set(GtkColorButton* btn, gpointer _off)
{
    gsize off = GPOINTER_TO_SIZE(_off);
    GdkColor* val = (GdkColor*)G_STRUCT_MEMBER_P(fm_config, off);
    GdkColor new_val;
    gtk_color_button_get_color(btn, &new_val);
    if( !gdk_color_equal(val, &new_val) )
    {
        const char* name = g_object_get_data((GObject*)btn, "changed");
        if(!name)
            name = gtk_buildable_get_name((GtkBuildable*)btn);
        *val = new_val;
        fm_config_emit_changed(fm_config, name);
    }
}

static void init_color(GtkBuilder* b, const char* name, gsize off, const char* changed_notify)
{
    GtkColorButton* btn = GTK_COLOR_BUTTON(gtk_builder_get_object(b, name));
    GdkColor* val = (GdkColor*)G_STRUCT_MEMBER_P(fm_config, off);
    if(changed_notify)
        g_object_set_data_full(G_OBJECT(btn), "changed", g_strdup(changed_notify), g_free);
    gtk_color_button_set_color(btn, val);
    g_signal_connect(btn, "color-set", G_CALLBACK(on_color_set), GSIZE_TO_POINTER(off));
}

static void on_wallpaper_set(GtkFileChooserButton* btn, gpointer user_data)
{
    char* file = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(btn));
    g_free(app_config->wallpaper);
    app_config->wallpaper = file;
    fm_config_emit_changed(fm_config, "wallpaper");
}

static void on_update_img_preview( GtkFileChooser *chooser, GtkImage* img )
{
    char* file = gtk_file_chooser_get_preview_filename( chooser );
    GdkPixbuf* pix = NULL;
    if( file )
    {
        pix = gdk_pixbuf_new_from_file_at_scale( file, 128, 128, TRUE, NULL );
        g_free( file );
    }
    if( pix )
    {
        gtk_file_chooser_set_preview_widget_active(chooser, TRUE);
        gtk_image_set_from_pixbuf( img, pix );
        g_object_unref( pix );
    }
    else
    {
        gtk_image_clear( img );
        gtk_file_chooser_set_preview_widget_active(chooser, FALSE);
    }
}

static void on_desktop_font_set(GtkFontButton* btn, gpointer user_data)
{
    const char* font = gtk_font_button_get_font_name(btn);
    if(font)
    {
        g_free(app_config->desktop_font);
        app_config->desktop_font = g_strdup(font);
        fm_config_emit_changed(fm_config, "desktop_font");
    }
}

void fm_desktop_preference(GtkAction* act, GtkWindow* parent)
{
    if(!desktop_pref_dlg)
    {
        GtkBuilder* builder = gtk_builder_new();
        GtkWidget* item, *img_preview;
        gtk_builder_add_from_file(builder, PACKAGE_UI_DIR "/desktop-pref.ui", NULL);
        desktop_pref_dlg = GTK_WINDOW(gtk_builder_get_object(builder, "dlg"));

        item = (GtkWidget*)gtk_builder_get_object(builder, "wallpaper");
        g_signal_connect(item, "file-set", G_CALLBACK(on_wallpaper_set), NULL);
        img_preview = gtk_image_new();
        gtk_misc_set_alignment(GTK_MISC(img_preview), 0.5, 0.0);
        gtk_widget_set_size_request( img_preview, 128, 128 );
        gtk_file_chooser_set_preview_widget(GTK_FILE_CHOOSER(item), img_preview);
        g_signal_connect( item, "update-preview", G_CALLBACK(on_update_img_preview), img_preview );
        if(app_config->wallpaper)
            gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(item), app_config->wallpaper);

        INIT_COMBO(builder, FmAppConfig, wallpaper_mode, "wallpaper");
        INIT_COLOR(builder, FmAppConfig, desktop_bg, "wallpaper");
        INIT_BOOL(builder, FmAppConfig, wallpaper_common, "wallpaper");

        INIT_COLOR(builder, FmAppConfig, desktop_fg, "desktop_text");
        INIT_COLOR(builder, FmAppConfig, desktop_shadow, "desktop_text");

        INIT_BOOL(builder, FmAppConfig, show_wm_menu, NULL);

        INIT_COMBO(builder, FmAppConfig, arrange_icons_rtl, "arrange_icons_rtl");
        INIT_COMBO(builder, FmAppConfig, arrange_icons_btt, "arrange_icons_btt");
        INIT_COMBO(builder, FmAppConfig, arrange_icons_in_rows, "arrange_icons_in_rows");
        INIT_INT(builder, FmAppConfig, desktop_icon_size, "desktop_icon_size");

        item = (GtkWidget*)gtk_builder_get_object(builder, "desktop_font");
        if(app_config->desktop_font)
            gtk_font_button_set_font_name(GTK_FONT_BUTTON(item), app_config->desktop_font);
        g_signal_connect(item, "font-set", G_CALLBACK(on_desktop_font_set), NULL);

        g_signal_connect(desktop_pref_dlg, "response", G_CALLBACK(on_response), &desktop_pref_dlg);
        g_object_unref(builder);

        pcmanfm_ref();
        g_signal_connect(desktop_pref_dlg, "destroy", G_CALLBACK(pcmanfm_unref), NULL);
        if(parent)
            gtk_window_set_transient_for(desktop_pref_dlg, parent);
    }
    gtk_window_present(desktop_pref_dlg);
}
