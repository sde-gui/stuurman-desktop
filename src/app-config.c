/*
 *      app-config.c
 *
 *      Copyright 2010 PCMan <pcman.tw@gmail.com>
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

#include <libsmfm-gtk/fm-gtk.h>
#include <stdio.h>

#include "app-config.h"
#include "pcmanfm.h"

#define APP_CONFIG_NAME "desktop.conf"

static void fm_app_config_finalize              (GObject *object);

G_DEFINE_TYPE(FmAppConfig, fm_app_config, FM_CONFIG_TYPE);


static void fm_app_config_class_init(FmAppConfigClass *klass)
{
    GObjectClass *g_object_class;
    g_object_class = G_OBJECT_CLASS(klass);
    g_object_class->finalize = fm_app_config_finalize;
}


static void fm_app_config_finalize(GObject *object)
{
    FmAppConfig *cfg;

    g_return_if_fail(object != NULL);
    g_return_if_fail(IS_FM_APP_CONFIG(object));

    cfg = FM_APP_CONFIG(object);
    if(cfg->wallpapers_configured > 0)
    {
        int i;

        for(i = 0; i < cfg->wallpapers_configured; i++)
            g_free(cfg->wallpapers[i]);
        g_free(cfg->wallpapers);
    }
    g_free(cfg->wallpaper);
    g_free(cfg->desktop_font);

    G_OBJECT_CLASS(fm_app_config_parent_class)->finalize(object);
}


static void fm_app_config_init(FmAppConfig *cfg)
{
    /* load libfm config file */
    fm_config_load_from_file((FmConfig*)cfg, NULL);

    cfg->desktop_fg.red = cfg->desktop_fg.green = cfg->desktop_fg.blue = 65535;

    gdk_color_parse("#3A6EA5", &cfg->desktop_bg);

    cfg->desktop_sort_type = GTK_SORT_ASCENDING;
    cfg->desktop_sort_by = COL_FILE_MTIME;

    cfg->wallpaper_common = TRUE;
}


FmConfig *fm_app_config_new(void)
{
    return (FmConfig*)g_object_new(FM_APP_CONFIG_TYPE, NULL);
}

void fm_app_config_load_from_key_file(FmAppConfig* cfg, GKeyFile* kf)
{
    char* tmp;
    int tmp_int;

    /* desktop */
    if(fm_key_file_get_int(kf, "desktop", "wallpaper_mode", &tmp_int))
        cfg->wallpaper_mode = (FmWallpaperMode)tmp_int;

    if(cfg->wallpapers_configured > 0)
    {
        int i;

        for(i = 0; i < cfg->wallpapers_configured; i++)
            g_free(cfg->wallpapers[i]);
        g_free(cfg->wallpapers);
    }
    g_free(cfg->wallpaper);
    cfg->wallpaper = NULL;
    fm_key_file_get_int(kf, "desktop", "wallpapers_configured", &cfg->wallpapers_configured);
    if(cfg->wallpapers_configured > 0)
    {
        char wpn_buf[32];
        int i;

        cfg->wallpapers = g_malloc(cfg->wallpapers_configured * sizeof(char *));
        for(i = 0; i < cfg->wallpapers_configured; i++)
        {
            snprintf(wpn_buf, sizeof(wpn_buf), "wallpaper%d", i);
            tmp = g_key_file_get_string(kf, "desktop", wpn_buf, NULL);
            cfg->wallpapers[i] = tmp;
        }
    }
    fm_key_file_get_bool(kf, "desktop", "wallpaper_common", &cfg->wallpaper_common);
    if (cfg->wallpaper_common)
    {
        tmp = g_key_file_get_string(kf, "desktop", "wallpaper", NULL);
        cfg->wallpaper = tmp;
    }

    tmp = g_key_file_get_string(kf, "desktop", "desktop_bg", NULL);
    if(tmp)
    {
        gdk_color_parse(tmp, &cfg->desktop_bg);
        g_free(tmp);
    }
    tmp = g_key_file_get_string(kf, "desktop", "desktop_fg", NULL);
    if(tmp)
    {
        gdk_color_parse(tmp, &cfg->desktop_fg);
        g_free(tmp);
    }
    tmp = g_key_file_get_string(kf, "desktop", "desktop_shadow", NULL);
    if(tmp)
    {
        gdk_color_parse(tmp, &cfg->desktop_shadow);
        g_free(tmp);
    }

    tmp = g_key_file_get_string(kf, "desktop", "desktop_font", NULL);
    g_free(cfg->desktop_font);
    cfg->desktop_font = tmp;

    fm_key_file_get_bool(kf, "desktop", "show_wm_menu", &cfg->show_wm_menu);
    if(fm_key_file_get_int(kf, "desktop", "sort_type", &tmp_int) &&
       tmp_int == GTK_SORT_DESCENDING)
        cfg->desktop_sort_type = GTK_SORT_DESCENDING;
    else
        cfg->desktop_sort_type = GTK_SORT_ASCENDING;
    if(fm_key_file_get_int(kf, "desktop", "sort_by", &tmp_int) &&
       FM_FOLDER_MODEL_COL_IS_VALID((guint)tmp_int))
        cfg->desktop_sort_by = tmp_int;

    fm_key_file_get_bool(kf, "desktop", "arrange_icons_rtl", &cfg->arrange_icons_rtl);
}

void fm_app_config_load_from_profile(FmAppConfig* cfg, const char* name)
{
    const gchar * const *dirs, * const *dir;
    char *path;
    GKeyFile* kf = g_key_file_new();

    if(!name || !*name) /* if profile name is not provided, use 'default' */
        name = "default";

    /* load system-wide settings */
    dirs = g_get_system_config_dirs();
    for(dir=dirs;*dir;++dir)
    {
        path = g_build_filename(*dir, config_app_name(), name, APP_CONFIG_NAME, NULL);
        if(g_key_file_load_from_file(kf, path, 0, NULL))
            fm_app_config_load_from_key_file(cfg, kf);
        g_free(path);
    }

    /* override system-wide settings with user-specific configuration */

    /* For backward compatibility, try to load old config file and
     * then migrate to new location */
    path = g_build_filename(g_get_user_config_dir(), config_app_name(), name, APP_CONFIG_NAME, NULL);
    if(g_key_file_load_from_file(kf, path, 0, NULL))
        fm_app_config_load_from_key_file(cfg, kf);
    g_free(path);

    g_key_file_free(kf);

    /* set some additional default values when needed. */
    if(!cfg->desktop_font) /* set a proper desktop font if needed */
        cfg->desktop_font = g_strdup("Sans 12");
}

void fm_app_config_save_profile(FmAppConfig* cfg, const char* name)
{
    char* path = NULL;;
    char* dir_path;

    if(!name || !*name)
        name = "default";

    dir_path = g_build_filename(g_get_user_config_dir(), config_app_name(), name, NULL);
    if(g_mkdir_with_parents(dir_path, 0700) != -1)
    {
        GString* buf = g_string_sized_new(1024);

        g_string_append(buf, "\n[desktop]\n");
        g_string_append_printf(buf, "wallpaper_mode=%d\n", cfg->wallpaper_mode);
        g_string_append_printf(buf, "wallpaper_common=%d\n", cfg->wallpaper_common);
        if (cfg->wallpapers && cfg->wallpapers_configured > 0)
        {
            int i;

            g_string_append_printf(buf, "wallpapers_configured=%d\n", cfg->wallpapers_configured);
            for (i = 0; i < cfg->wallpapers_configured; i++)
                if (cfg->wallpapers[i])
                    g_string_append_printf(buf, "wallpaper%d=%s\n", i, cfg->wallpapers[i]);
        }
        if (cfg->wallpaper_common)
            g_string_append_printf(buf, "wallpaper=%s\n", cfg->wallpaper ? cfg->wallpaper : "");
        //FIXME: should desktop_bg and wallpaper_mode be set for each desktop too?
        g_string_append_printf(buf, "desktop_bg=#%02x%02x%02x\n", cfg->desktop_bg.red/257, cfg->desktop_bg.green/257, cfg->desktop_bg.blue/257);
        g_string_append_printf(buf, "desktop_fg=#%02x%02x%02x\n", cfg->desktop_fg.red/257, cfg->desktop_fg.green/257, cfg->desktop_fg.blue/257);
        g_string_append_printf(buf, "desktop_shadow=#%02x%02x%02x\n", cfg->desktop_shadow.red/257, cfg->desktop_shadow.green/257, cfg->desktop_shadow.blue/257);
        if(cfg->desktop_font && *cfg->desktop_font)
            g_string_append_printf(buf, "desktop_font=%s\n", cfg->desktop_font);
        g_string_append_printf(buf, "show_wm_menu=%d\n", cfg->show_wm_menu);
        g_string_append_printf(buf, "sort_type=%d\n", cfg->desktop_sort_type);
        g_string_append_printf(buf, "sort_by=%d\n", cfg->desktop_sort_by);
        g_string_append_printf(buf, "arrange_icons_rtl=%d\n", cfg->arrange_icons_rtl);

        path = g_build_filename(dir_path, APP_CONFIG_NAME, NULL);
        g_file_set_contents(path, buf->str, buf->len, NULL);
        g_string_free(buf, TRUE);
        g_free(path);
    }
    g_free(dir_path);
}

