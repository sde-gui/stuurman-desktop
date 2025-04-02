/*
 *      desktop.c
 *
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

#include "desktop.h"
#include "pcmanfm.h"
#include "app-config.h"

#include <glib/gi18n.h>

#include <gdk/gdkx.h>
#include <gdk/gdkkeysyms.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <math.h>

#include <cairo-xlib.h>

#include "pref.h"

#include "gseal-gtk-compat.h"

typedef struct _FmBackgroundCache FmBackgroundCache;
typedef struct _FmBackgroundCacheParams FmBackgroundCacheParams;

struct _FmBackgroundCacheParams
{
    char *filename;
    FmWallpaperMode wallpaper_mode;
    int dest_w;
    int dest_h;
    GdkColor desktop_bg;
};

struct _FmBackgroundCache
{
    FmBackgroundCache *next;
    int desktop_nr;
    FmBackgroundCacheParams params;
#if GTK_CHECK_VERSION(3, 0, 0)
    cairo_surface_t *bg;
#else
    GdkPixmap *bg;
#endif
};

static Atom XA_NET_WORKAREA = 0;
static Atom XA_NET_NUMBER_OF_DESKTOPS = 0;
static Atom XA_NET_CURRENT_DESKTOP = 0;
static Atom XA_XROOTMAP_ID = 0;
static Atom XA_XROOTPMAP_ID = 0;

static FmBackgroundCache* all_wallpapers = NULL;

static const char * get_wallpaper_path(guint cur_desktop, gboolean on_wallpaper_changed)
{
    if (app_config->wallpaper_common)
        return app_config->wallpaper;

    const char *wallpaper_path;

    if (on_wallpaper_changed) /* signal "changed::wallpaper" */
    {
        if ((gint)cur_desktop >= app_config->wallpapers_configured)
        {
            register int i;

            app_config->wallpapers = g_renew(char *, app_config->wallpapers, cur_desktop + 1);
            for(i = MAX(app_config->wallpapers_configured,0); i <= (gint)cur_desktop; i++)
                app_config->wallpapers[i] = NULL;
            app_config->wallpapers_configured = cur_desktop + 1;
        }
        wallpaper_path = app_config->wallpaper;
        g_free(app_config->wallpapers[cur_desktop]);
        app_config->wallpapers[cur_desktop] = g_strdup(wallpaper_path);
    }
    else /* desktop refresh */
    {
        if ((gint)cur_desktop < app_config->wallpapers_configured)
            wallpaper_path = app_config->wallpapers[cur_desktop];
        else
            wallpaper_path = NULL;
        if (app_config->wallpaper)
        {
            g_free(app_config->wallpaper); /* update to current desktop */
        }
        app_config->wallpaper = g_strdup(wallpaper_path);
    }

    return wallpaper_path;
}

static gboolean params_equal(const FmBackgroundCacheParams *a, const FmBackgroundCacheParams *b)
{
    if (a->wallpaper_mode != b->wallpaper_mode)
        return FALSE;

    if (memcmp(&a->desktop_bg, &b->desktop_bg, sizeof(a->desktop_bg)) != 0)
        return FALSE;

    switch(a->wallpaper_mode)
    {
    case FM_WP_COLOR:
        break;
    case FM_WP_TILE:
        if (g_strcmp0(a->filename, b->filename) != 0)
            return FALSE;
        break;
    default:
        if (a->dest_w != b->dest_w)
            return FALSE;
        if (a->dest_h != b->dest_h)
            return FALSE;
        if (g_strcmp0(a->filename, b->filename) != 0)
            return FALSE;
        break;
    }

    return TRUE;
}

static void get_desktop_size(FmDesktop* desktop, int *w, int *h)
{
    GtkWidget* widget = (GtkWidget*)desktop;
    GdkScreen* screen = gtk_widget_get_screen(widget);
    GdkRectangle geom;
    gdk_screen_get_monitor_geometry(screen, desktop->monitor, &geom);
    *w = geom.width;
    *h = geom.height;
}

static FmBackgroundCache *lookup_cache(int desktop_nr)
{
    FmBackgroundCache *cache;
    for (cache = all_wallpapers; cache; cache = cache->next)
    {
        if (cache->desktop_nr == desktop_nr)
            break;
    }

    if (!cache)
    {
        cache = g_new0(FmBackgroundCache, 1);
        cache->next = all_wallpapers;
        all_wallpapers = cache;
        cache->desktop_nr = desktop_nr;
    }

    return cache;
}

static void prepare_cached_background(FmBackgroundCache *cache, GdkWindow *window)
{
    GdkPixbuf* pix = gdk_pixbuf_new_from_file(cache->params.filename, NULL);
    int src_w, src_h;
    int dest_w, dest_h;
    int x = 0, y = 0;
    src_w = gdk_pixbuf_get_width(pix);
    src_h = gdk_pixbuf_get_height(pix);
    if (cache->params.wallpaper_mode == FM_WP_TILE)
    {
        dest_w = src_w;
        dest_h = src_h;
    }
    else
    {
        dest_w = cache->params.dest_w;
        dest_h = cache->params.dest_h;
    }

#if GTK_CHECK_VERSION(3, 0, 0)
    cache->bg = cairo_image_surface_create(CAIRO_FORMAT_RGB24, dest_w, dest_h);
    cairo_t* cr = cairo_create(cache->bg);
#else
    cache->bg = gdk_pixmap_new(window, dest_w, dest_h, -1);
    cairo_t* cr = gdk_cairo_create(cache->bg);
#endif
    if (gdk_pixbuf_get_has_alpha(pix)
       || app_config->wallpaper_mode == FM_WP_CENTER
       || app_config->wallpaper_mode == FM_WP_FIT)
    {
        gdk_cairo_set_source_color(cr, &cache->params.desktop_bg);
        cairo_rectangle(cr, 0, 0, dest_w, dest_h);
        cairo_fill(cr);
    }

    switch(app_config->wallpaper_mode)
    {
        case FM_WP_TILE:
            break;
        case FM_WP_STRETCH:
        {
            GdkPixbuf *scaled;
            if (dest_w == src_w && dest_h == src_h)
                scaled = (GdkPixbuf*)g_object_ref(pix);
            else
                scaled = gdk_pixbuf_scale_simple(pix, dest_w, dest_h, GDK_INTERP_BILINEAR);
            g_object_unref(pix);
            pix = scaled;
            break;
        }
        case FM_WP_FIT:
        {
            if (dest_w != src_w || dest_h != src_h)
            {
                gdouble w_ratio = (float)dest_w / src_w;
                gdouble h_ratio = (float)dest_h / src_h;
                gdouble ratio = MIN(w_ratio, h_ratio);
                if(ratio != 1.0)
                {
                    src_w *= ratio;
                    src_h *= ratio;
                    GdkPixbuf *scaled = gdk_pixbuf_scale_simple(
                        pix, src_w, src_h, GDK_INTERP_BILINEAR);
                    g_object_unref(pix);
                    pix = scaled;
                }
            }
            x = (dest_w - src_w) / 2;
            y = (dest_h - src_h) / 2;
            break;
        }
        case FM_WP_CENTER:
        {
            x = (dest_w - src_w) / 2;
            y = (dest_h - src_h) / 2;
            break;
        }
        case FM_WP_COLOR: ; /* handled outside of this function */
    }
    gdk_cairo_set_source_pixbuf(cr, pix, x, y);
    cairo_paint(cr);
    cairo_destroy(cr);

    if (pix)
        g_object_unref(pix);
}

void wallpaper_manager_update_background(FmDesktop* desktop, gboolean on_wallpaper_changed)
{
    GtkWidget* widget = (GtkWidget*)desktop;
    GdkWindow* root = gdk_screen_get_root_window(gtk_widget_get_screen(widget));
    GdkWindow *window = gtk_widget_get_window(widget);

    Display* xdisplay;
    Pixmap xpixmap = 0;
    Window xroot;
    int screen_num;

    if (app_config->wallpaper_mode == FM_WP_COLOR)
    {
#if GTK_CHECK_VERSION(3, 0, 0)
        cairo_pattern_t *pattern;
        pattern = cairo_pattern_create_rgb(app_config->desktop_bg.red / 65535.0,
                                           app_config->desktop_bg.green / 65535.0,
                                           app_config->desktop_bg.blue / 65535.0);
        gdk_window_set_background_pattern(window, pattern);
        cairo_pattern_destroy(pattern);
#else
        GdkColor bg = app_config->desktop_bg;

        gdk_colormap_alloc_color(gdk_drawable_get_colormap(window), &bg, FALSE, TRUE);
        gdk_window_set_back_pixmap(window, NULL, FALSE);
        gdk_window_set_background(window, &bg);
#endif
        gdk_window_invalidate_rect(window, NULL, TRUE);
        return;
    }

    FmBackgroundCache *cache = lookup_cache(desktop->cur_desktop);

    const char *wallpaper_path = get_wallpaper_path(desktop->cur_desktop, on_wallpaper_changed);
    FmBackgroundCacheParams params;
    params.filename = (char*) wallpaper_path;
    params.wallpaper_mode = app_config->wallpaper_mode;
    params.desktop_bg = app_config->desktop_bg;
    get_desktop_size(desktop, &params.dest_w, &params.dest_h);

    if (!params_equal(&cache->params, &params))
    {
        /* the same file but mode was changed */
        if (cache->bg)
        {
#if GTK_CHECK_VERSION(3, 0, 0)
            cairo_surface_destroy(cache->bg);
#else
            g_object_unref(cache->bg);
#endif
            cache->bg = NULL;
        }

        g_free(cache->params.filename);
        cache->params = params;
        cache->params.filename = g_strdup(params.filename);
    }

    if (!cache->bg) /* no cached image found */
        prepare_cached_background(cache, window);

#if GTK_CHECK_VERSION(3, 0, 0)
    cairo_pattern_t *pattern;
    pattern = cairo_pattern_create_for_surface(cache->bg);
    gdk_window_set_background_pattern(window, pattern);
    cairo_pattern_destroy(pattern);
#else
    gdk_window_set_back_pixmap(window, cache->bg, FALSE);
#endif

    /* set root map here */
    screen_num = gdk_screen_get_number(gtk_widget_get_screen(widget));
    xdisplay = GDK_WINDOW_XDISPLAY(root);
    xroot = RootWindow(xdisplay, screen_num);

#if GTK_CHECK_VERSION(3, 0, 0)
    xpixmap = cairo_xlib_surface_get_drawable(cache->bg);
#else
    xpixmap = GDK_WINDOW_XWINDOW(cache->bg);
#endif

    XChangeProperty(xdisplay, GDK_WINDOW_XID(root),
                    XA_XROOTMAP_ID, XA_PIXMAP, 32, PropModeReplace, (guchar*)&xpixmap, 1);

    XGrabServer (xdisplay);

#if 0
    result = XGetWindowProperty (display,
                                 RootWindow (display, screen_num),
                                 gdk_x11_get_xatom_by_name ("ESETROOT_PMAP_ID"),
                                 0L, 1L, False, XA_PIXMAP,
                                 &type, &format, &nitems,
                                 &bytes_after,
                                 &data_esetroot);

    if (data_esetroot != NULL) {
            if (result == Success && type == XA_PIXMAP &&
                format == 32 &&
                nitems == 1) {
                    gdk_error_trap_push ();
                    XKillClient (display, *(Pixmap *)data_esetroot);
                    gdk_error_trap_pop_ignored ();
            }
            XFree (data_esetroot);
    }

    XChangeProperty (display, RootWindow (display, screen_num),
                     gdk_x11_get_xatom_by_name ("ESETROOT_PMAP_ID"),
                     XA_PIXMAP, 32, PropModeReplace,
                     (guchar *) &xpixmap, 1);
#endif

    XChangeProperty(xdisplay, xroot, XA_XROOTPMAP_ID, XA_PIXMAP, 32,
                    PropModeReplace, (guchar*)&xpixmap, 1);

    XSetWindowBackgroundPixmap(xdisplay, xroot, xpixmap);
    XClearWindow(xdisplay, xroot);

    XFlush(xdisplay);
    XUngrabServer(xdisplay);

    gdk_window_invalidate_rect(window, NULL, TRUE);
}

void wallpaper_manager_init()
{
    char* atom_names[] = {"_NET_WORKAREA", "_NET_NUMBER_OF_DESKTOPS",
                          "_NET_CURRENT_DESKTOP", "_XROOTMAP_ID", "_XROOTPMAP_ID"};
    Atom atoms[G_N_ELEMENTS(atom_names)] = {0};

    if(XInternAtoms(gdk_x11_get_default_xdisplay(), atom_names,
                    G_N_ELEMENTS(atom_names), False, atoms))
    {
        XA_NET_WORKAREA = atoms[0];
        XA_NET_NUMBER_OF_DESKTOPS = atoms[1];
        XA_NET_CURRENT_DESKTOP = atoms[2];
        XA_XROOTMAP_ID = atoms[3];
        XA_XROOTPMAP_ID = atoms[4];
    }

}

void wallpaper_manager_finalize()
{
    while(all_wallpapers)
    {
        FmBackgroundCache *bg = all_wallpapers;

        all_wallpapers = bg->next;
#if GTK_CHECK_VERSION(3, 0, 0)
        cairo_surface_destroy(bg->bg);
#else
        g_object_unref(bg->bg);
#endif
        g_free(bg->params.filename);
        g_free(bg);
    }
}
