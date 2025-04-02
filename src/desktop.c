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
#include "desktop-manager.h"
#include "pcmanfm.h"
#include "app-config.h"
#include "wallpaper-manager.h"
#include "window-tracker.h"

#include <glib/gi18n.h>

#include <gdk/gdkx.h>
#include <gdk/gdkkeysyms.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <math.h>

#include <cairo-xlib.h>

#include "pref.h"

#include "gseal-gtk-compat.h"

#include "cell-placement-generator.h"


#define SPACING 2
#define PADDING 6
#define MARGIN  2

typedef struct _cached_layout_image
{
    guint timestamp;
    cairo_surface_t * surface;
} cached_layout_image_t;

static inline void cached_layout_image_invalidate(cached_layout_image_t * cache)
{
    if (cache->surface)
    {
        cairo_surface_destroy(cache->surface);
        cache->surface = NULL;
    }
}

static inline gboolean cached_layout_image_check_timestamp(cached_layout_image_t * cache, guint timestamp)
{
    if (cache->timestamp != timestamp)
        cached_layout_image_invalidate(cache);
    return cache->surface != NULL;
}

struct _FmDesktopItem
{
    FmFileInfo* fi;
    int x; /* position of the item on the desktop */
    int y;
    GdkRectangle icon_rect;
    GdkRectangle text_rect;

    PangoRectangle text_pango_logical_rect;
    guint pango_timestamp;

    cached_layout_image_t cached_text;
    cached_layout_image_t cached_text_shadow;

    gboolean is_special : 1; /* is this a special item like "My Computer", mounted volume, or "Trash" */
    gboolean is_mount : 1; /* is this a mounted volume*/
    gboolean is_selected : 1;
    gboolean is_prelight : 1;
    gboolean fixed_pos : 1;
};

static void queue_layout_items(FmDesktop* desktop);

static FmFileInfoList* _dup_selected_files(FmFolderView* fv);
static FmPathList* _dup_selected_file_paths(FmFolderView* fv);
static void _select_all(FmFolderView* fv);
static void _unselect_all(FmFolderView* fv);
static void _select_invert(FmFolderView* fv);

static void fm_desktop_view_init(FmFolderViewInterface* iface);

G_DEFINE_TYPE_WITH_CODE(FmDesktop, fm_desktop, GTK_TYPE_WINDOW,
                        G_IMPLEMENT_INTERFACE(FM_TYPE_FOLDER_VIEW, fm_desktop_view_init))


static Atom XA_NET_WORKAREA = 0;
static Atom XA_NET_NUMBER_OF_DESKTOPS = 0;
static Atom XA_NET_CURRENT_DESKTOP = 0;
static Atom XA_XROOTMAP_ID = 0;
static Atom XA_XROOTPMAP_ID = 0;

static GdkCursor* hand_cursor = NULL;

enum {
#if N_FM_DND_DEST_DEFAULT_TARGETS > N_FM_DND_SRC_DEFAULT_TARGETS
    FM_DND_DEST_DESKTOP_ITEM = N_FM_DND_DEST_DEFAULT_TARGETS
#else
    FM_DND_DEST_DESKTOP_ITEM = N_FM_DND_SRC_DEFAULT_TARGETS
#endif
};

GtkTargetEntry dnd_targets[] =
{
    {"application/x-desktop-item", GTK_TARGET_SAME_WIDGET, FM_DND_DEST_DESKTOP_ITEM}
};

GdkAtom desktop_atom;

enum
{
    PROP_0,
    PROP_MONITOR,
    N_PROPERTIES
};

/* popup menu callbacks */
static void on_open_in_new_win(GtkAction* act, gpointer user_data);
static void on_open_in_new_tab(GtkAction* act, gpointer user_data);
static void on_open_folder_in_terminal(GtkAction* act, gpointer user_data);

static void on_fix_pos(GtkToggleAction* act, gpointer user_data);
static void on_snap_to_grid(GtkAction* act, gpointer user_data);

static void on_show_icons(GtkToggleAction* act, gpointer user_data);

/* insert GtkUIManager XML definitions */
#include "desktop-ui.c"


#define CONTINUE_IF_ITEM_IS_NULL(item) \
    if (!item) \
    {\
        g_debug("item is NULL");\
        continue;\
    }\


/* ---------------------------------------------------------------------
    Items management and common functions */

static char* get_config_file(FmDesktop* desktop, gboolean create_dir)
{
    int screen_n = gdk_screen_get_number(gtk_widget_get_screen(GTK_WIDGET(desktop)));

    gchar * dir = pcmanfm_get_profile_dir(create_dir);
    gchar * path = g_strdup_printf("%s/desktop-items-%u-%u.conf", dir, screen_n, desktop->monitor);

    g_free(dir);

    return path;
}

static inline FmDesktopItem* desktop_item_new(FmFolderModel* model, GtkTreeIter* it)
{
    FmDesktopItem* item = g_slice_new0(FmDesktopItem);
    fm_folder_model_set_item_userdata(model, it, item);
    gtk_tree_model_get(GTK_TREE_MODEL(model), it, COL_FILE_INFO, &item->fi, -1);
    fm_file_info_ref(item->fi);
    return item;
}

static inline void desktop_item_free(FmDesktopItem* item)
{
    if (!item)
        return;

    if (item->fi)
        fm_file_info_unref(item->fi);
    g_slice_free(FmDesktopItem, item);
}

static void calc_item_size(FmDesktop* desktop, FmDesktopItem* item, GdkPixbuf* icon)
{
    /* icon rect */
    if(icon)
    {
        item->icon_rect.width = gdk_pixbuf_get_width(icon);
        item->icon_rect.height = gdk_pixbuf_get_height(icon);
        item->icon_rect.x = item->x + (desktop->cell_w - item->icon_rect.width) / 2;
        item->icon_rect.y = item->y + desktop->ypad + (app_config->desktop_icon_size - item->icon_rect.height) / 2;
        item->icon_rect.height += desktop->spacing;
    }
    else
    {
        item->icon_rect.width = app_config->desktop_icon_size;
        item->icon_rect.height = app_config->desktop_icon_size;
        item->icon_rect.x = item->x + desktop->ypad;
        item->icon_rect.y = item->y + desktop->ypad;
        item->icon_rect.height += desktop->spacing;
    }

    /* text label rect */

    if (item->pango_timestamp != desktop->pango_timestamp)
    {
        item->pango_timestamp = desktop->pango_timestamp;

        pango_layout_set_text(desktop->pl, NULL, 0);

        pango_layout_set_height(desktop->pl, desktop->pango_text_h);
        pango_layout_set_width(desktop->pl, desktop->pango_text_w);
        pango_layout_set_text(desktop->pl, fm_file_info_get_disp_name(item->fi), -1);

        PangoRectangle _unused_rc;
        pango_layout_get_pixel_extents(desktop->pl, &_unused_rc, &item->text_pango_logical_rect);
        pango_layout_set_text(desktop->pl, NULL, 0);

        cached_layout_image_invalidate(&item->cached_text);
        cached_layout_image_invalidate(&item->cached_text_shadow);
    }

    item->text_rect.x = item->x + (desktop->cell_w - item->text_pango_logical_rect.width - 4) / 2;
    item->text_rect.y = item->icon_rect.y + item->icon_rect.height + item->text_pango_logical_rect.y;
    item->text_rect.width = item->text_pango_logical_rect.width + 4;
    item->text_rect.height = item->text_pango_logical_rect.height + 4;
}

void load_items(FmDesktop* desktop)
{
    GtkTreeIter it;
    char* path;
    GtkTreeModel* model = GTK_TREE_MODEL(desktop->model);
    GKeyFile* kf;

    if(!gtk_tree_model_get_iter_first(model, &it))
        return;
    path = get_config_file(desktop, FALSE);
    if(!path)
        return;
    kf = g_key_file_new();
    if(g_key_file_load_from_file(kf, path, 0, NULL))
    {
        do
        {
            FmDesktopItem* item;
            const char* name;
            GdkPixbuf* icon = NULL;

            item = fm_folder_model_get_item_userdata(desktop->model, &it);
            CONTINUE_IF_ITEM_IS_NULL(item);
            name = fm_file_info_get_name(item->fi);
            if(g_key_file_has_group(kf, name))
            {
                gtk_tree_model_get(model, &it, FM_FOLDER_MODEL_COL_ICON_WITH_THUMBNAIL, &icon, -1);
                desktop->fixed_items = g_list_prepend(desktop->fixed_items, item);
                item->fixed_pos = TRUE;
                item->x = g_key_file_get_integer(kf, name, "x", NULL);
                item->y = g_key_file_get_integer(kf, name, "y", NULL);
                calc_item_size(desktop, item, icon);
                if(icon)
                    g_object_unref(icon);
            }
        }
        while(gtk_tree_model_iter_next(model, &it));
    }
    g_free(path);
    g_key_file_free(kf);
    queue_layout_items(desktop);
}

void unload_items(FmDesktop* desktop)
{
    /* remove existing fixed items */
    g_list_free(desktop->fixed_items);
    desktop->fixed_items = NULL;
    desktop->focus = NULL;
    desktop->drop_hilight = NULL;
    desktop->hover_item = NULL;
}

static gint get_desktop_for_root_window(GdkWindow *root)
{
    gint desktop = -1;
    Atom ret_type;
    gulong len, after;
    int format;
    guchar* prop;

    if(XGetWindowProperty(GDK_WINDOW_XDISPLAY(root), GDK_WINDOW_XID(root),
                          XA_NET_CURRENT_DESKTOP, 0, 1, False, XA_CARDINAL, &ret_type,
                          &format, &len, &after, &prop) == Success &&
       prop != NULL)
    {
        desktop = (gint)*(guint32*)prop;
        XFree(prop);
    }
    return desktop;
}

/* save position of desktop icons */
void save_item_pos(FmDesktop* desktop)
{
    GList* l;
    GString* buf;
    char* path = get_config_file(desktop, TRUE);
    if(!path)
        return;
    buf = g_string_sized_new(1024);
    for(l = desktop->fixed_items; l; l=l->next)
    {
        FmDesktopItem* item = (FmDesktopItem*)l->data;
        FmPath* fi_path = fm_file_info_get_path(item->fi);
        const char* p;
        /* write the file basename as group name */
        g_string_append_c(buf, '[');
        for(p = fm_path_get_basename(fi_path); *p; ++p)
        {
            switch(*p)
            {
            case '\r':
                g_string_append(buf, "\\r");
                break;
            case '\n':
                g_string_append(buf, "\\n");
                break;
            case '\\':
                g_string_append(buf, "\\\\");
                break;
            default:
                g_string_append_c(buf, *p);
            }
        }
        g_string_append(buf, "]\n");
        g_string_append_printf(buf, "x=%d\n"
                                    "y=%d\n\n",
                                    item->x, item->y);
    }
    g_file_set_contents(path, buf->str, buf->len, NULL);
    g_free(path);
    g_string_free(buf, TRUE);
}

static GList* get_selected_items(FmDesktop* desktop, int* n_items)
{
    GList* items = NULL;
    int n = 0;
    FmDesktopItem* focus = NULL;
    GtkTreeModel* model = GTK_TREE_MODEL(desktop->model);
    GtkTreeIter it;
    if(gtk_tree_model_get_iter_first(model, &it)) do
    {
        FmDesktopItem* item = fm_folder_model_get_item_userdata(desktop->model, &it);
        CONTINUE_IF_ITEM_IS_NULL(item);
        if(item->is_selected)
        {
            if(G_LIKELY(item != desktop->focus))
            {
                items = g_list_prepend(items, item);
                ++n;
            }
            else
                focus = item;
        }
    }
    while(gtk_tree_model_iter_next(model, &it));
    items = g_list_reverse(items);
    if(focus)
    {
        items = g_list_prepend(items, focus);
        ++n;
    }
    if(n_items)
        *n_items = n;
    return items;
}


/* ---------------------------------------------------------------------
    Desktop drawing */

static void calculate_item_metrics(FmDesktop* self)
{
    PangoContext* pc;
    PangoFontMetrics *metrics;
    int font_h;
    pc = gtk_widget_get_pango_context((GtkWidget*)self);

    metrics = pango_context_get_metrics(pc, NULL, NULL);

    font_h = pango_font_metrics_get_ascent(metrics) + pango_font_metrics_get_descent (metrics);
    pango_font_metrics_unref(metrics);

    font_h /= PANGO_SCALE;

    self->spacing = SPACING;
    self->xpad = self->ypad = PADDING;
    self->xmargin = self->ymargin = MARGIN;

    /* FIXME: font_h * 2 doesn't give 2 lines in layout. Figure out how to do it properly */
    /*self->text_h = font_h * 2;*/
    self->text_h = font_h * 2.4;

    /* Add more space for text, if icons are big enough */
    self->text_h = MAX(self->text_h, app_config->desktop_icon_size * 0.2);

    self->text_w = 100;
    /* Add more space for text, if icons are big enough */
    self->text_w = MAX(self->text_w, app_config->desktop_icon_size * 1.2);

    self->pango_text_h = self->text_h * PANGO_SCALE;
    self->pango_text_w = self->text_w * PANGO_SCALE;
    self->text_h += 4;
    self->text_w += 4; /* 4 is for drawing border */
    self->cell_h = app_config->desktop_icon_size + self->spacing + self->text_h + self->ypad * 2;
    self->cell_w = MAX((gint)self->text_w, app_config->desktop_icon_size) + self->xpad * 2;
}

static inline void get_item_rect(FmDesktopItem* item, GdkRectangle* rect)
{
    gdk_rectangle_union(&item->icon_rect, &item->text_rect, rect);
}

static gboolean is_pos_occupied(FmDesktop* desktop, FmDesktopItem* item)
{
    GList* l;
    for(l = desktop->fixed_items; l; l=l->next)
    {
        FmDesktopItem* fixed = (FmDesktopItem*)l->data;
        GdkRectangle rect;
        get_item_rect(fixed, &rect);
        if(gdk_rectangle_intersect(&rect, &item->icon_rect, NULL)
         ||gdk_rectangle_intersect(&rect, &item->text_rect, NULL))
            return TRUE;
    }

    return fm_window_tracker_test_overlap(&item->icon_rect) || fm_window_tracker_test_overlap(&item->text_rect);
}

static void layout_items(FmDesktop* self)
{
    FmDesktopItem* item;
    GtkTreeModel* model = GTK_TREE_MODEL(self->model);
    GdkPixbuf* icon;
    GtkTreeIter it;

    calculate_item_metrics(self);

    if(!gtk_tree_model_get_iter_first(model, &it))
    {
        gtk_widget_queue_draw(GTK_WIDGET(self));
        return;
    }

    CellPlacementGenerator cpg;
    cell_placement_generator_set_bounding_box(&cpg,
        self->working_area.x + self->xmargin,
        self->working_area.y + self->ymargin,
        self->working_area.x + self->working_area.width - self->xmargin,
        self->working_area.y + self->working_area.height - self->ymargin
    );
    cell_placement_generator_set_cell_size(&cpg,
        self->cell_w,
        self->cell_h
    );
    cell_placement_generator_set_placement_rules(&cpg,
        app_config->arrange_icons_in_rows,
        app_config->arrange_icons_rtl,
        app_config->arrange_icons_btt
    );

    cell_placement_generator_reset(&cpg);

    do
    {
        item = fm_folder_model_get_item_userdata(self->model, &it);
        CONTINUE_IF_ITEM_IS_NULL(item);

        icon = NULL;
        gtk_tree_model_get(model, &it, FM_FOLDER_MODEL_COL_ICON_WITH_THUMBNAIL, &icon, -1);
        if (item->fixed_pos)
        {
            calc_item_size(self, item, icon);
        }
        else
        {
_next_position:
            item->x = cpg.x;
            item->y = cpg.y;
            calc_item_size(self, item, icon);

            cell_placement_generator_advance(&cpg);

            /* check if this position is occupied by a fixed item */
            if (is_pos_occupied(self, item))
                goto _next_position;
        }
        if (icon)
            g_object_unref(icon);
    }
    while(gtk_tree_model_iter_next(model, &it));

    gtk_widget_queue_draw(GTK_WIDGET(self));
}

static gboolean on_idle_layout(FmDesktop* desktop)
{
    desktop->idle_layout = 0;
    layout_items(desktop);
    return FALSE;
}

static void queue_layout_items(FmDesktop* desktop)
{
    if(0 == desktop->idle_layout)
        desktop->idle_layout = g_idle_add((GSourceFunc)on_idle_layout, desktop);
}

static void paint_item_text(FmDesktop* self, FmDesktopItem* item, cached_layout_image_t * cache, float blur_radius, cairo_t* cr)
{
    cairo_save(cr);

//g_print("%d, %d\n", item->text_pango_logical_rect.x, item->text_pango_logical_rect.y);

    if (!cached_layout_image_check_timestamp(cache, self->pango_timestamp))
    {
        cache->timestamp = self->pango_timestamp;
        g_debug("creating cached surface %dx%d", item->text_rect.width, item->text_rect.height);
        cache->surface = cairo_surface_create_similar(cairo_get_target(cr),
            //CAIRO_CONTENT_COLOR_ALPHA,
            CAIRO_CONTENT_ALPHA,
            item->text_rect.width + item->text_pango_logical_rect.x,
            item->text_rect.height + item->text_pango_logical_rect.y);

        cairo_t * cr2 = cairo_create(cache->surface);
        cairo_set_source_rgb(cr2, 1, 1, 1);
        cairo_move_to(cr2, 0, 0);
        pango_cairo_update_layout(cr2, self->pl);
        pango_cairo_show_layout(cr2, self->pl);

        cairo_destroy(cr2);

        if (blur_radius > 0)
        {
            cairo_surface_t * surface = cairo_surface_create_similar(cairo_get_target(cr),
                //CAIRO_CONTENT_COLOR_ALPHA,
                CAIRO_CONTENT_ALPHA,
                item->text_rect.width + item->text_pango_logical_rect.x,
                item->text_rect.height + item->text_pango_logical_rect.y);

            cairo_t * cr3 = cairo_create(surface);
            cairo_set_source_rgba(cr3, 1, 1, 1, 0.3);

            float r = blur_radius;

            cairo_mask_surface(cr3, cache->surface, 0, 0);
            cairo_fill(cr3);

            cairo_mask_surface(cr3, cache->surface, 0, r);
            cairo_fill(cr3);

            cairo_mask_surface(cr3, cache->surface, 0, -r);
            cairo_fill(cr3);

            cairo_mask_surface(cr3, cache->surface, r, 0);
            cairo_fill(cr3);

            cairo_mask_surface(cr3, cache->surface, -r, 0);
            cairo_fill(cr3);

            float c45 = 0.7 * r;

            cairo_mask_surface(cr3, cache->surface, c45, c45);
            cairo_fill(cr3);

            cairo_mask_surface(cr3, cache->surface, c45, -c45);
            cairo_fill(cr3);

            cairo_mask_surface(cr3, cache->surface, -c45, c45);
            cairo_fill(cr3);

            cairo_mask_surface(cr3, cache->surface, -c45, -c45);
            cairo_fill(cr3);

            cairo_destroy(cr3);

            cairo_surface_destroy(cache->surface);
            cache->surface = surface;
        }
    }

    double x, y;
    cairo_get_current_point(cr, &x, &y);
    cairo_mask_surface(cr, cache->surface, x, y);
    cairo_fill(cr);

    cairo_restore(cr);
}


static void paint_item(FmDesktop* self, FmDesktopItem* item, cairo_t* cr, GdkRectangle* expose_area, GdkPixbuf* icon, gdouble item_opacity)
{
#if GTK_CHECK_VERSION(3, 0, 0)
    GtkStyleContext* style;
#else
    GtkStyle* style;
#endif
    GtkWidget* widget = (GtkWidget*)self;
    GtkCellRendererState state = 0;
#if GTK_CHECK_VERSION(3, 0, 0)
    GdkRGBA rgba;
#else
    GdkWindow* window;
#endif
    int text_x, text_y;

#if GTK_CHECK_VERSION(3, 0, 0)
    style = gtk_widget_get_style_context(widget);
#else
    style = gtk_widget_get_style(widget);
    window = gtk_widget_get_window(widget);
#endif

    pango_layout_set_text(self->pl, NULL, 0);
    pango_layout_set_width(self->pl, self->pango_text_w);
    pango_layout_set_height(self->pl, self->pango_text_h);

    pango_layout_set_text(self->pl, fm_file_info_get_disp_name(item->fi), -1);

    /* FIXME: do we need to cache this? */
    text_x = item->x + (self->cell_w - self->text_w)/2 + 2;
    text_y = item->icon_rect.y + item->icon_rect.height + 2;

    if(item->is_selected || item == self->drop_hilight) /* draw background for text label */
    {
        state = GTK_CELL_RENDERER_SELECTED;

        cairo_save(cr);
        gdk_cairo_rectangle(cr, &item->text_rect);
#if GTK_CHECK_VERSION(3, 0, 0)
        gtk_style_context_get_background_color(style, GTK_STATE_FLAG_SELECTED, &rgba);
        gdk_cairo_set_source_rgba(cr, &rgba);
#else
        gdk_cairo_set_source_color(cr, &style->bg[GTK_STATE_SELECTED]);
#endif
        cairo_clip(cr);
        cairo_paint(cr);
        cairo_restore(cr);
#if GTK_CHECK_VERSION(3, 0, 0)
        gtk_style_context_get_color(style, GTK_STATE_FLAG_SELECTED, &rgba);
        gdk_cairo_set_source_rgba(cr, &rgba);
#else
        gdk_cairo_set_source_color(cr, &style->fg[GTK_STATE_SELECTED]);
#endif
    }
    else
    {
        /* the shadow */

        float shadow_offset = 0;
        float shadow_blur_radius = 0;

        /* TODO: should user-configurable */

        // Thin
        //shadow_offset = 1.0;
        //shadow_blur_radius = 0.0;

        // Normal
        shadow_offset = 1.0;
        shadow_blur_radius = 0.7;

        // Bold
        //shadow_offset = 1.3;
        //shadow_blur_radius = 1.5;

        // Bold 2
        //shadow_offset = 1.5;
        //shadow_blur_radius = 2.0;

        gdk_cairo_set_source_color(cr, &app_config->desktop_shadow);
        cairo_move_to(cr, text_x + shadow_offset, text_y + shadow_offset);
        //pango_cairo_show_layout(cr, self->pl);
        paint_item_text(self, item, &item->cached_text_shadow, shadow_blur_radius, cr);
        gdk_cairo_set_source_color(cr, &app_config->desktop_fg);
    }

    /* real text */
    cairo_move_to(cr, text_x, text_y);
    /* FIXME: should we check if pango is 1.10 at least? */
    //pango_cairo_show_layout(cr, self->pl);
    paint_item_text(self, item, &item->cached_text, 0, cr);
    pango_layout_set_text(self->pl, NULL, 0);

    if (item == self->focus && gtk_window_is_active((GtkWindow *) self))
    {
#if GTK_CHECK_VERSION(3, 0, 0)
        gtk_render_focus(style, cr,
#else
        gtk_paint_focus(style, window, gtk_widget_get_state(widget),
                        expose_area, widget, "icon_view",
#endif
                        item->text_rect.x, item->text_rect.y, item->text_rect.width, item->text_rect.height);
    }

    /* draw the icon */
    g_object_set(self->icon_render, "pixbuf", icon, "info", item->fi, NULL);
#if GTK_CHECK_VERSION(3, 0, 0)
    gtk_cell_renderer_render(GTK_CELL_RENDERER(self->icon_render), cr, widget, &item->icon_rect, &item->icon_rect, state);
#else
    gtk_cell_renderer_render(GTK_CELL_RENDERER(self->icon_render), window, widget, &item->icon_rect, &item->icon_rect, expose_area, state);
#endif
}

static void redraw_item(FmDesktop* desktop, FmDesktopItem* item)
{
    GdkRectangle rect;
    gdk_rectangle_union(&item->icon_rect, &item->text_rect, &rect);
    --rect.x;
    --rect.y;
    rect.width += 2;
    rect.height += 2;
    gdk_window_invalidate_rect(gtk_widget_get_window(GTK_WIDGET(desktop)), &rect, FALSE);
}

static void move_item(FmDesktop* desktop, FmDesktopItem* item, int x, int y, gboolean redraw)
{
    int dx, dy;
    /* this call invalid the area occupied by the item and a redraw
     * is queued. */
    if(redraw)
        redraw_item(desktop, item);

    dx = x - item->x;
    dy = y - item->y;

    item->x = x;
    item->y = y;

    /* calc_item_size(desktop, item); */
    item->icon_rect.x += dx;
    item->icon_rect.y += dy;
    item->text_rect.x += dx;
    item->text_rect.y += dy;

    /* make the item use customized fixed position. */
    if(!item->fixed_pos)
    {
        item->fixed_pos = TRUE;
        desktop->fixed_items = g_list_prepend(desktop->fixed_items, item);
    }

    /* move the item to a new place, and queue a redraw for the new rect. */
    if(redraw)
        redraw_item(desktop, item);

#if 0
    /* check if the item is overlapped with another item */
    for(l = desktop->items; l; l=l->next)
    {
        FmDesktopItem* item2 = (FmDesktopItem*)l->data;
    }
#endif
}

static void calc_rubber_banding_rect(FmDesktop* self, int x, int y, GdkRectangle* rect)
{
    int x1, x2, y1, y2;
    if(self->drag_start_x < x)
    {
        x1 = self->drag_start_x;
        x2 = x;
    }
    else
    {
        x1 = x;
        x2 = self->drag_start_x;
    }

    if(self->drag_start_y < y)
    {
        y1 = self->drag_start_y;
        y2 = y;
    }
    else
    {
        y1 = y;
        y2 = self->drag_start_y;
    }

    rect->x = x1;
    rect->y = y1;
    rect->width = x2 - x1;
    rect->height = y2 - y1;
}

static void update_rubberbanding(FmDesktop* self, int newx, int newy)
{
    if (!app_config->show_icons)
        return;

    GtkTreeModel* model = GTK_TREE_MODEL(self->model);
    GtkTreeIter it;
    GdkRectangle old_rect, new_rect;
    //GdkRegion *region;
    GdkWindow *window;

    window = gtk_widget_get_window(GTK_WIDGET(self));

    calc_rubber_banding_rect(self, self->rubber_banding_x, self->rubber_banding_y, &old_rect);
    calc_rubber_banding_rect(self, newx, newy, &new_rect);

    gdk_window_invalidate_rect(window, &old_rect, FALSE);
    gdk_window_invalidate_rect(window, &new_rect, FALSE);
//    gdk_window_clear_area(((GtkWidget*)self)->window, new_rect.x, new_rect.y, new_rect.width, new_rect.height);
/*
    region = gdk_region_rectangle(&old_rect);
    gdk_region_union_with_rect(region, &new_rect);

//    gdk_window_invalidate_region(((GtkWidget*)self)->window, &region, TRUE);

    gdk_region_destroy(region);
*/
    self->rubber_banding_x = newx;
    self->rubber_banding_y = newy;

    /* update selection */
    if(gtk_tree_model_get_iter_first(model, &it)) do
    {
        FmDesktopItem* item = fm_folder_model_get_item_userdata(self->model, &it);
        CONTINUE_IF_ITEM_IS_NULL(item);

        gboolean selected;
        if(gdk_rectangle_intersect(&new_rect, &item->icon_rect, NULL) ||
            gdk_rectangle_intersect(&new_rect, &item->text_rect, NULL))
            selected = TRUE;
        else
            selected = FALSE;

        if(item->is_selected != selected)
        {
            item->is_selected = selected;
            redraw_item(self, item);
        }
    }
    while(gtk_tree_model_iter_next(model, &it));
}


static void paint_rubber_banding_rect(FmDesktop* self, cairo_t* cr, GdkRectangle* expose_area)
{
    GtkWidget* widget = (GtkWidget*)self;
    GdkRectangle rect;
    GdkColor clr;
    guchar alpha;

    calc_rubber_banding_rect(self, self->rubber_banding_x, self->rubber_banding_y, &rect);

    if(rect.width <= 0 || rect.height <= 0)
        return;

    if(!gdk_rectangle_intersect(expose_area, &rect, &rect))
        return;
/*
    gtk_widget_style_get(icon_view,
                        "selection-box-color", &clr,
                        "selection-box-alpha", &alpha,
                        NULL);
*/
    clr = gtk_widget_get_style (widget)->base[GTK_STATE_SELECTED];
    alpha = 64;  /* FIXME: should be themable in the future */

    cairo_save(cr);
    cairo_set_source_rgba(cr, (gdouble)clr.red/65535, (gdouble)clr.green/65536, (gdouble)clr.blue/65535, (gdouble)alpha/100);
    gdk_cairo_rectangle(cr, &rect);
    cairo_clip (cr);
    cairo_paint (cr);
    gdk_cairo_set_source_color(cr, &clr);
    cairo_rectangle (cr, rect.x + 0.5, rect.y + 0.5, rect.width - 1, rect.height - 1);
    cairo_stroke(cr);
    cairo_restore(cr);
}

/* ---------------------------------------------------------------------
    FmFolder signal handlers */


/* ---------------------------------------------------------------------
    FmFolderModel signal handlers */

static void on_row_deleting(FmFolderModel* model, GtkTreePath* tp,
                            GtkTreeIter* iter, gpointer data, FmDesktop* desktop)
{
    GList *l;

    desktop_item_free(data);
    for(l = desktop->fixed_items; l; l = l->next)
        if(l->data == data)
        {
            desktop->fixed_items = g_list_delete_link(desktop->fixed_items, l);
            break;
        }
    if((gpointer)desktop->focus == data)
    {
        GtkTreeIter it = *iter;
        if(gtk_tree_model_iter_next(GTK_TREE_MODEL(model), &it))
            desktop->focus = fm_folder_model_get_item_userdata(model, &it);
        else
        {
            if(gtk_tree_path_prev(tp))
            {
                gtk_tree_model_get_iter(GTK_TREE_MODEL(model), &it, tp);
                gtk_tree_path_next(tp);
                desktop->focus = fm_folder_model_get_item_userdata(model, &it);
            }
            else
                desktop->focus = NULL;
        }
    }
    if((gpointer)desktop->drop_hilight == data)
        desktop->drop_hilight = NULL;
    if((gpointer)desktop->hover_item == data)
        desktop->hover_item = NULL;
}

static void on_row_inserted(FmFolderModel* mod, GtkTreePath* tp, GtkTreeIter* it, FmDesktop* desktop)
{
    FmDesktopItem* item = desktop_item_new(mod, it);
    fm_folder_model_set_item_userdata(mod, it, item);
    queue_layout_items(desktop);
}

static void on_row_deleted(FmFolderModel* mod, GtkTreePath* tp, FmDesktop* desktop)
{
    queue_layout_items(desktop);
}

static void on_row_changed(FmFolderModel* model, GtkTreePath* tp, GtkTreeIter* it, FmDesktop* desktop)
{
    do {
        FmDesktopItem* item = fm_folder_model_get_item_userdata(model, it);
        CONTINUE_IF_ITEM_IS_NULL(item);

        fm_file_info_unref(item->fi);
        gtk_tree_model_get(GTK_TREE_MODEL(model), it, COL_FILE_INFO, &item->fi, -1);
        fm_file_info_ref(item->fi);

        redraw_item(desktop, item);
        /* queue_layout_items(desktop); */
    } while (0);
}

static void on_rows_reordered(FmFolderModel* model, GtkTreePath* parent_tp, GtkTreeIter* parent_it, gpointer arg3, FmDesktop* desktop)
{
    queue_layout_items(desktop);
}


/* ---------------------------------------------------------------------
    Events handlers */

static gboolean transition_worker(FmDesktop* desktop)
{
    gboolean schedule_next_frame = FALSE;

    if (app_config->show_icons)
    {
        if (desktop->show_icons_transition_current < desktop->show_icons_transition_interval)
        {
            desktop->show_icons_transition_current += desktop->transition_update_interval;
            gtk_widget_queue_draw(GTK_WIDGET(desktop));
            schedule_next_frame = TRUE;
        }
    }
    else if (!app_config->show_icons)
    {
        if (desktop->show_icons_transition_current > 0)
        {
            desktop->show_icons_transition_current -= desktop->transition_update_interval;
            gtk_widget_queue_draw(GTK_WIDGET(desktop));
            schedule_next_frame = TRUE;
        }
    }

    if (!schedule_next_frame)
        desktop->transition_worker_handler_id = 0;
    return schedule_next_frame;
}

static void start_transition(FmDesktop* desktop)
{
    if (!desktop->transition_worker_handler_id)
    {
        desktop->transition_worker_handler_id = g_timeout_add(
            desktop->transition_update_interval,
            (GSourceFunc) transition_worker,
            (gpointer) desktop);
    }
}

static void update_working_area(FmDesktop* desktop)
{
    GdkScreen* screen = gtk_widget_get_screen((GtkWidget*)desktop);
    GdkRectangle result;
#if GTK_CHECK_VERSION(3, 4, 0)
    gdk_screen_get_monitor_workarea(screen, desktop->monitor, &result);
#else
    /*
    FIXME: All this is WRONG.

    _NET_WORKAREA is calculated diffently by different WMs (openbox vs icewm,
    for example), and is not suitable for multimonitor configuration any way.

    We have to manually enumerate top-level windows and check their
    _NET_WM_STRUT and _NET_WM_STRUT_PARTIAL.

    See also:
    https://muktupavels.id.lv/posts/better-multi-monitor-support-in-gnome-flashback
    */


    GdkWindow* root = gdk_screen_get_root_window(screen);
    Atom ret_type;
    gulong len, after;
    int format;
    guchar* prop;
    guint32 n_desktops, cur_desktop;
    gulong* working_area;

    GdkRectangle monitor_geometry;
    gdk_screen_get_monitor_geometry(screen, desktop->monitor, &monitor_geometry);

    if(XGetWindowProperty(GDK_WINDOW_XDISPLAY(root), GDK_WINDOW_XID(root),
                       XA_NET_NUMBER_OF_DESKTOPS, 0, 1, False, XA_CARDINAL, &ret_type,
                       &format, &len, &after, &prop) != Success)
        goto _out;
    if(!prop)
        goto _out;
    n_desktops = *(guint32*)prop;
    XFree(prop);

    if(XGetWindowProperty(GDK_WINDOW_XDISPLAY(root), GDK_WINDOW_XID(root),
                       XA_NET_CURRENT_DESKTOP, 0, 1, False, XA_CARDINAL, &ret_type,
                       &format, &len, &after, &prop) != Success)
        goto _out;
    if(!prop)
        goto _out;
    cur_desktop = *(guint32*)prop;
    XFree(prop);

    if(XGetWindowProperty(GDK_WINDOW_XDISPLAY(root), GDK_WINDOW_XID(root),
                       XA_NET_WORKAREA, 0, 4 * 32, False, AnyPropertyType, &ret_type,
                       &format, &len, &after, &prop) != Success)
        goto _out;
    if(ret_type == None || format == 0 || len != n_desktops*4)
    {
        if(prop)
            XFree(prop);
        goto _out;
    }
    working_area = ((gulong*)prop) + cur_desktop * 4;

    GdkRectangle wm_working_area;
    wm_working_area.x      = (gint) working_area[0];
    wm_working_area.y      = (gint) working_area[1];
    wm_working_area.width  = (gint) working_area[2];
    wm_working_area.height = (gint) working_area[3];

    XFree(prop);

    int monitor_geometry_right  = monitor_geometry.x + monitor_geometry.width;
    int monitor_geometry_bottom = monitor_geometry.y + monitor_geometry.height;

    int wm_working_area_right  = wm_working_area.x + wm_working_area.width;
    int wm_working_area_bottom = wm_working_area.y + wm_working_area.height;

    int left   = MAX(monitor_geometry.x, wm_working_area.x);
    int top    = MAX(monitor_geometry.y, wm_working_area.y);
    int right  = MIN(monitor_geometry_right, wm_working_area_right);
    int bottom = MIN(monitor_geometry_bottom, wm_working_area_bottom);

    result.x      = left;
    result.y      = top;
    result.width  = right - left;
    result.height = bottom - top;

    if (result.width < 1 || result.height < 1)
        result = monitor_geometry;

    result.x -= monitor_geometry.x;
    result.y -= monitor_geometry.y;

_out:
#endif

    if (desktop->working_area.x != result.x
    ||  desktop->working_area.y != result.y
    ||  desktop->working_area.width != result.width
    ||  desktop->working_area.height != result.height)
    {
        desktop->working_area = result;
        queue_layout_items(desktop);
    }

    g_debug("got working area: %d.%d.%d.%d", desktop->working_area.x, desktop->working_area.y,
            desktop->working_area.width, desktop->working_area.height);

}

static GdkFilterReturn on_root_event(GdkXEvent *xevent, GdkEvent *event, gpointer data)
{
    XPropertyEvent * evt = (XPropertyEvent*) xevent;
    FmDesktop* self = (FmDesktop*)data;
    if (evt->type == PropertyNotify)
    {
        if(evt->atom == XA_NET_WORKAREA)
            update_working_area(self);
        else if(evt->atom == XA_NET_CURRENT_DESKTOP)
        {
            gint desktop = get_desktop_for_root_window(gdk_screen_get_root_window(
                                    gtk_widget_get_screen(GTK_WIDGET(data))));
            if(desktop >= 0)
            {
                self->cur_desktop = (guint)desktop;
                if(!app_config->wallpaper_common)
                    wallpaper_manager_update_background(self, -1);
            }
        }
    }
    return GDK_FILTER_CONTINUE;
}

static void on_screen_size_changed(GdkScreen* screen, FmDesktop* desktop)
{
    GdkRectangle geom;
    gdk_screen_get_monitor_geometry(screen, desktop->monitor, &geom);
    gtk_window_resize((GtkWindow *) desktop, geom.width, geom.height);
    gtk_window_set_default_size((GtkWindow *) desktop, geom.width, geom.height);
    gtk_window_move((GtkWindow *) desktop, geom.x, geom.y);
}

static void on_screen_monitors_changed(GdkScreen* screen, FmDesktop* desktop)
{
    on_screen_size_changed(screen, desktop);
}

/* ---------------------------------------------------------------------
    Popup handlers */

static void fm_desktop_update_popup(FmFolderView* fv, GtkWindow* window,
                                    GtkUIManager* ui, GtkActionGroup* act_grp,
                                    FmFileInfoList* files)
{
    GtkAction* act;

    FmDesktop* desktop = FM_DESKTOP(fv);
    desktop->popup_act_grp = act_grp;

    /* remove 'Rename' item and accelerator */
    act = gtk_action_group_get_action(act_grp, "Rename");
    gtk_action_set_visible(act, FALSE);
    gtk_action_set_sensitive(act, FALSE);
    /* hide 'Show Hidden' item */
    act = gtk_action_group_get_action(act_grp, "ShowHidden");
    gtk_action_set_visible(act, FALSE);
    /* add 'Configure desktop' item replacing 'Properties' */
    act = gtk_action_group_get_action(act_grp, "Prop");
    gtk_action_set_visible(act, FALSE);
    //gtk_action_group_remove_action(act_grp, act);
    gtk_action_group_set_translation_domain(act_grp, NULL);

    gtk_action_group_add_actions(act_grp,
        desktop_actions, G_N_ELEMENTS(desktop_actions), window);
    gtk_action_group_add_toggle_actions(act_grp,
        desktop_toggle_actions,
        G_N_ELEMENTS(desktop_toggle_actions), fv);

    {
        GtkAction * action = gtk_action_group_get_action(act_grp, "ShowIcons");
        gtk_toggle_action_set_active(GTK_TOGGLE_ACTION(action), app_config->show_icons);
    }

    gtk_ui_manager_add_ui_from_string(ui, desktop_menu_xml, -1, NULL);
}

static void fm_desktop_update_item_popup(FmFolderView* fv, GtkWindow* window,
                                         GtkUIManager* ui, GtkActionGroup* act_grp,
                                         FmFileInfoList* files)
{
    FmFileInfo* fi;
    GList* sel_items, *l;
    GtkAction* act;
    gboolean all_fixed = TRUE, has_fixed = FALSE;

    sel_items = get_selected_items(FM_DESKTOP(fv), NULL);
    for(l = sel_items; l; l=l->next)
    {
        FmDesktopItem* item = (FmDesktopItem*)l->data;
        if(item->fixed_pos)
            has_fixed = TRUE;
        else
            all_fixed = FALSE;
    }
    g_list_free(sel_items);

    fi = (FmFileInfo*)fm_file_info_list_peek_head(files);

    /* merge some specific menu items for folders */
    gtk_action_group_set_translation_domain(act_grp, NULL);
    if(fm_file_info_list_get_length(files) == 1 && fm_file_info_is_directory(fi))
    {
        gtk_action_group_add_actions(act_grp, folder_menu_actions,
                                     G_N_ELEMENTS(folder_menu_actions), fv);
        gtk_ui_manager_add_ui_from_string(ui, folder_menu_xml, -1, NULL);
    }

    /* merge desktop icon specific items */
    gtk_action_group_add_actions(act_grp, desktop_icon_actions,
                                 G_N_ELEMENTS(desktop_icon_actions), fv);
    act = gtk_action_group_get_action(act_grp, "Snap");
    gtk_action_set_sensitive(act, has_fixed);

    gtk_action_group_add_toggle_actions(act_grp, desktop_icon_toggle_actions,
                                        G_N_ELEMENTS(desktop_icon_toggle_actions),
                                        fv);
    act = gtk_action_group_get_action(act_grp, "Fix");
    gtk_toggle_action_set_active(GTK_TOGGLE_ACTION(act), all_fixed);

    gtk_ui_manager_add_ui_from_string(ui, desktop_icon_menu_xml, -1, NULL);
}

/* folder options work only with single folder - see above */
static void on_open_in_new_tab(GtkAction* act, gpointer user_data)
{/*
    FmDesktop* desktop = FM_DESKTOP(user_data);

    if(desktop->focus)
        fm_main_win_open_in_last_active(fm_file_info_get_path(desktop->focus->fi));*/
}

static void on_open_in_new_win(GtkAction* act, gpointer user_data)
{
/*    FmDesktop* desktop = FM_DESKTOP(user_data);

    if(desktop->focus)
        fm_main_win_add_win(NULL, fm_file_info_get_path(desktop->focus->fi));*/
}

static void on_open_folder_in_terminal(GtkAction* act, gpointer user_data)
{
#if 0
    FmDesktop* desktop = FM_DESKTOP(user_data);

    if(desktop->focus /*&& !fm_file_info_is_virtual(fi)*/)
        pcmanfm_open_folder_in_terminal(NULL, fm_file_info_get_path(desktop->focus->fi));
#endif
}

static void on_fix_pos(GtkToggleAction* act, gpointer user_data)
{
    FmDesktop* desktop = FM_DESKTOP(user_data);
    GList* items = get_selected_items(desktop, NULL);
    GList* l;
    if(gtk_toggle_action_get_active(act))
    {
        for(l = items; l; l=l->next)
        {
            FmDesktopItem* item = (FmDesktopItem*)l->data;
            if(!item->fixed_pos)
            {
                item->fixed_pos = TRUE;
                desktop->fixed_items = g_list_prepend(desktop->fixed_items, item);
            }
        }
    }
    else
    {
        for(l = items; l; l=l->next)
        {
            FmDesktopItem* item = (FmDesktopItem*)l->data;
            item->fixed_pos = FALSE;
            desktop->fixed_items = g_list_remove(desktop->fixed_items, item);
        }
        layout_items(desktop);
    }
    g_list_free(items);
    save_item_pos(desktop);
}

/* round() is only available in C99. Don't use it now for portability. */
static inline double _round(double x)
{
    return (x > 0.0) ? floor(x + 0.5) : ceil(x - 0.5);
}

static void on_snap_to_grid(GtkAction* act, gpointer user_data)
{
    FmDesktop* desktop = FM_DESKTOP(user_data);
    FmDesktopItem* item;
    GList* items = get_selected_items(desktop, NULL);
    GList* l;
    int x, y;
    GtkTextDirection direction = gtk_widget_get_direction(GTK_WIDGET(desktop));

    y = desktop->working_area.y + desktop->ymargin;
    //bottom = desktop->working_area.y + desktop->working_area.height - desktop->ymargin - desktop->cell_h;

    if(direction != GTK_TEXT_DIR_RTL) /* LTR or NONE */
        x = desktop->working_area.x + desktop->xmargin;
    else /* RTL */
        x = desktop->working_area.x + desktop->working_area.width - desktop->xmargin - desktop->cell_w;

    for(l = items; l; l = l->next)
    {
        int new_x, new_y;
        item = (FmDesktopItem*)l->data;
        if(!item->fixed_pos)
            continue;
        new_x = x + _round((double)(item->x - x) / desktop->cell_w) * desktop->cell_w;
        new_y = y + _round((double)(item->y - y) / desktop->cell_h) * desktop->cell_h;
        move_item(desktop, item, new_x, new_y, FALSE);
    }
    g_list_free(items);

    queue_layout_items(desktop);
}

static void on_show_icons(GtkToggleAction* act, gpointer user_data)
{
    gboolean show_icons = act ? gtk_toggle_action_get_active(act) : !app_config->show_icons;

    if (app_config->show_icons == show_icons)
        return;

    app_config->show_icons = show_icons;
    fm_config_emit_changed(fm_config, "show_icons");
}

/* ---------------------------------------------------------------------
    GtkWidget class default signal handlers */

static gboolean is_point_in_rect(GdkRectangle* rect, int x, int y)
{
    return rect->x < x && x < (rect->x + rect->width) && y > rect->y && y < (rect->y + rect->height);
}

static FmDesktopItem* hit_test(FmDesktop* self, GtkTreeIter *it, int x, int y)
{
    if (!app_config->show_icons)
        return NULL;

    FmDesktopItem* item;
    GtkTreeModel* model = GTK_TREE_MODEL(self->model);
    if(gtk_tree_model_get_iter_first(model, it)) do
    {
        item = fm_folder_model_get_item_userdata(self->model, it);
        CONTINUE_IF_ITEM_IS_NULL(item);
        if(is_point_in_rect(&item->icon_rect, x, y)
         || is_point_in_rect(&item->text_rect, x, y))
            return item;
    }
    while(gtk_tree_model_iter_next(model, it));
    return NULL;
}

static FmDesktopItem* get_nearest_item(FmDesktop* desktop, FmDesktopItem* item,  GtkDirectionType dir)
{
    GtkTreeModel * model = GTK_TREE_MODEL(desktop->model);
    GtkTreeIter it;

    if(!gtk_tree_model_get_iter_first(model, &it))
        return NULL;
    if(!item) /* there is no focused item yet, select first one then */
        return fm_folder_model_get_item_userdata(desktop->model, &it);

    float d_left   = 1.5;
    float d_up     = 1.5;
    gboolean vertical = FALSE;

    switch(dir)
    {
        case GTK_DIR_LEFT:
            d_left = 1;
            vertical = FALSE;
            break;
        case GTK_DIR_RIGHT:
            d_left = -1;
            vertical = FALSE;
            break;
        case GTK_DIR_UP:
            d_up = 1;
            vertical = TRUE;
            break;
        case GTK_DIR_DOWN:
            d_up = -1;
            vertical = TRUE;
            break;
        case GTK_DIR_TAB_FORWARD: /* FIXME */
            break;
        case GTK_DIR_TAB_BACKWARD: /* FIXME */
            break;
    }

    FmDesktopItem * ret = NULL;
    float ret_distance = 0;

    do
    {
        FmDesktopItem * item2 = fm_folder_model_get_item_userdata(desktop->model, &it);
        CONTINUE_IF_ITEM_IS_NULL(item2);
        if (item2 == item)
            continue;

        float dx = (item->x - item2->x) * d_left;
        float dy = (item->y - item2->y) * d_up;

        if (!vertical && dx < 0)
            continue;

        if (vertical && dy < 0)
            continue;

        if (!vertical && dx == 0 && dy != 0)
            continue;

        if (vertical && dx != 0 && dy == 0)
            continue;

        float distance = dx * dx + dy * dy;

        if (!ret || distance < ret_distance)
        {
            ret = item2;
            ret_distance = distance;
        }
    }
    while(gtk_tree_model_iter_next(model, &it));

    return ret;
}

static gboolean has_selected_item(FmDesktop* desktop)
{
    GtkTreeModel* model = GTK_TREE_MODEL(desktop->model);
    GtkTreeIter it;
    if(gtk_tree_model_get_iter_first(model, &it)) do
    {
        FmDesktopItem* item = fm_folder_model_get_item_userdata(desktop->model, &it);
        CONTINUE_IF_ITEM_IS_NULL(item);
        if(item->is_selected)
            return TRUE;
    }
    while(gtk_tree_model_iter_next(model, &it));
    return FALSE;
}

static void set_focused_item(FmDesktop* desktop, FmDesktopItem* item)
{
    if(item != desktop->focus)
    {
        FmDesktopItem* old_focus = desktop->focus;
        desktop->focus = item;
        if(old_focus)
            redraw_item(desktop, old_focus);
        if(item)
            redraw_item(desktop, item);
    }
}

/* This function is taken from xfdesktop */
static void forward_event_to_rootwin(GdkScreen *gscreen, GdkEvent *event)
{
    XButtonEvent xev, xev2;
    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_screen_get_display(gscreen));

    if (event->type == GDK_BUTTON_PRESS || event->type == GDK_BUTTON_RELEASE)
    {
        if (event->type == GDK_BUTTON_PRESS)
        {
            xev.type = ButtonPress;
            /*
             * rox has an option to disable the next
             * instruction. it is called "blackbox_hack". Does
             * anyone know why exactly it is needed?
             */
            XUngrabPointer(dpy, event->button.time);
        }
        else
            xev.type = ButtonRelease;

        xev.button = event->button.button;
        xev.x = event->button.x;    /* Needed for icewm */
        xev.y = event->button.y;
        xev.x_root = event->button.x_root;
        xev.y_root = event->button.y_root;
        xev.state = event->button.state;

        xev2.type = 0;
    }
    else if (event->type == GDK_SCROLL)
    {
        xev.type = ButtonPress;
        xev.button = event->scroll.direction + 4;
        xev.x = event->scroll.x;    /* Needed for icewm */
        xev.y = event->scroll.y;
        xev.x_root = event->scroll.x_root;
        xev.y_root = event->scroll.y_root;
        xev.state = event->scroll.state;

        xev2.type = ButtonRelease;
        xev2.button = xev.button;
    }
    else
        return ;
    xev.window = GDK_WINDOW_XID(gdk_screen_get_root_window(gscreen));
    xev.root = xev.window;
    xev.subwindow = None;
    xev.time = event->button.time;
    xev.same_screen = True;

    XSendEvent(dpy, xev.window, False, ButtonPressMask | ButtonReleaseMask,
                (XEvent *) & xev);
    if (xev2.type == 0)
        return ;

    /* send button release for scroll event */
    xev2.window = xev.window;
    xev2.root = xev.root;
    xev2.subwindow = xev.subwindow;
    xev2.time = xev.time;
    xev2.x = xev.x;
    xev2.y = xev.y;
    xev2.x_root = xev.x_root;
    xev2.y_root = xev.y_root;
    xev2.state = xev.state;
    xev2.same_screen = xev.same_screen;

    XSendEvent(dpy, xev2.window, False, ButtonPressMask | ButtonReleaseMask,
                (XEvent *) & xev2);
}


#if GTK_CHECK_VERSION(3, 0, 0)
static gboolean on_draw(GtkWidget* w, cairo_t* cr)
#else
static gboolean on_expose(GtkWidget* w, GdkEventExpose* evt)
#endif
{
    FmDesktop* self = (FmDesktop*)w;
#if !GTK_CHECK_VERSION(3, 0, 0)
    cairo_t* cr;
#endif
    GtkTreeModel* model = GTK_TREE_MODEL(self->model);
    GtkTreeIter it;
    GdkRectangle area;

    gdouble item_opacity = self->show_icons_transition_current / (gdouble) self->show_icons_transition_interval;

#if GTK_CHECK_VERSION(3, 0, 0)
    if(G_UNLIKELY(!gtk_cairo_should_draw_window(cr, gtk_widget_get_window(w))))
        return FALSE;

    cairo_save(cr);
    gtk_cairo_transform_to_window(cr, w, gtk_widget_get_window(w));
    gdk_cairo_get_clip_rectangle(cr, &area);
#else
    if(G_UNLIKELY(! gtk_widget_get_visible (w) || ! gtk_widget_get_mapped (w)))
        return TRUE;

    cr = gdk_cairo_create(gtk_widget_get_window(w));
    area = evt->area;
#endif

    if (app_config->show_icons)
    {
        if(self->rubber_banding)
            paint_rubber_banding_rect(self, cr, &area);
    }

    if (item_opacity > 0.0)
    {
        if(gtk_tree_model_get_iter_first(model, &it)) do
        {
            FmDesktopItem* item = fm_folder_model_get_item_userdata(self->model, &it);
            CONTINUE_IF_ITEM_IS_NULL(item);

            GdkRectangle* intersect, tmp, tmp2;
            GdkPixbuf* icon = NULL;
            if(gdk_rectangle_intersect(&area, &item->icon_rect, &tmp))
                intersect = &tmp;
            else
                intersect = NULL;

            if(gdk_rectangle_intersect(&area, &item->text_rect, &tmp2))
            {
                if(intersect)
                    gdk_rectangle_union(intersect, &tmp2, intersect);
                else
                    intersect = &tmp2;
            }

            if(intersect)
            {
                gtk_tree_model_get(model, &it, FM_FOLDER_MODEL_COL_ICON_WITH_THUMBNAIL, &icon, -1);
                paint_item(self, item, cr, intersect, icon, item_opacity);
                if(icon)
                    g_object_unref(icon);
            }
        }
        while(gtk_tree_model_iter_next(model, &it));
    }

#if GTK_CHECK_VERSION(3, 0, 0)
    cairo_restore(cr);
#else
    cairo_destroy(cr);
#endif

    return TRUE;
}

static void on_size_allocate(GtkWidget* w, GtkAllocation* alloc)
{
    FmDesktop* self = (FmDesktop*)w;

    queue_layout_items(self);

    update_working_area(self);
    /* queue_layout_items(self); this is called in update_working_area */

    /* scale the wallpaper */
    if(gtk_widget_get_realized(w))
    {
        if(app_config->wallpaper_mode != FM_WP_COLOR && app_config->wallpaper_mode != FM_WP_TILE)
            wallpaper_manager_update_background(self, -1);
    }

    GTK_WIDGET_CLASS(fm_desktop_parent_class)->size_allocate(w, alloc);
}

#if GTK_CHECK_VERSION(3, 0, 0)
static void on_get_preferred_width(GtkWidget *w, gint *minimal_width, gint *natural_width)
{
    GdkScreen* scr = gtk_widget_get_screen(w);
    gint monitor = FM_DESKTOP(w)->monitor;
    GdkRectangle geom;
    gdk_screen_get_monitor_geometry(scr, monitor, &geom);
    *minimal_width = *natural_width = geom.width;
}

static void on_get_preferred_height(GtkWidget *w, gint *minimal_height, gint *natural_height)
{
    GdkScreen* scr = gtk_widget_get_screen(w);
    gint monitor = FM_DESKTOP(w)->monitor;
    GdkRectangle geom;
    gdk_screen_get_monitor_geometry(scr, monitor, &geom);
    *minimal_height = *natural_height = geom.height;
}
#else
static void on_size_request(GtkWidget* w, GtkRequisition* req)
{
    GdkScreen* scr = gtk_widget_get_screen(w);
    gint monitor = FM_DESKTOP(w)->monitor;
    GdkRectangle geom;
    gdk_screen_get_monitor_geometry(scr, monitor, &geom);
    req->width = geom.width;
    req->height = geom.height;
}
#endif

static gboolean on_button_press(GtkWidget* w, GdkEventButton* evt)
{
    //g_print("on_button_press\n");

    const int GDK_LEFT_BUTTON = 1;
    /*const int GDK_MIDDLE_BUTTON = 2;*/
    const int GDK_RIGHT_BUTTON = 3;

    FmDesktop* self = (FmDesktop*)w;
    FmDesktopItem *item = NULL, *clicked_item = NULL;
    GtkTreeIter it;
    FmFolderViewClickType clicked = FM_FV_CLICK_NONE;

    clicked_item = hit_test(FM_DESKTOP(w), &it, (int)evt->x, (int)evt->y);

    if(evt->type == GDK_BUTTON_PRESS)
    {
        if(evt->button == GDK_LEFT_BUTTON)
        {
            self->button_pressed = TRUE;    /* store button state for drag & drop */
            self->drag_start_x = evt->x;
            self->drag_start_y = evt->y;
        }

        /* if ctrl / shift is not pressed, deselect all. */
        /* FIXME: do [un]selection on button release */
        if(! (evt->state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK)))
        {
            /* don't cancel selection if clicking on selected items */
            if(!((evt->button == 1 || evt->button == 3) && clicked_item && clicked_item->is_selected))
                _unselect_all(FM_FOLDER_VIEW(self));
        }

        if(clicked_item)
        {
            if(evt->state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK))
                clicked_item->is_selected = ! clicked_item->is_selected;
            else
                clicked_item->is_selected = TRUE;

            if(self->focus && self->focus != item)
            {
                FmDesktopItem* old_focus = self->focus;
                self->focus = NULL;
                if(old_focus)
                    redraw_item(FM_DESKTOP(w), old_focus);
            }
            self->focus = clicked_item;
            redraw_item(self, clicked_item);

            if(evt->button == 3)  /* right click, context menu */
                clicked = FM_FV_CONTEXT_MENU;
            else if(evt->button == 2)
                clicked = FM_FV_MIDDLE_CLICK;
        }
        else /* no item is clicked */
        {
            if(evt->button == GDK_RIGHT_BUTTON)  /* right click on the blank area => desktop popup menu */
            {
                if (evt->state & GDK_CONTROL_MASK) /* always show menu if Ctrl is pressed... */
                    clicked = FM_FV_CONTEXT_MENU;
                else if (!app_config->show_wm_menu) /* ...else take the option in account  */
                    clicked = FM_FV_CONTEXT_MENU;
            }
            else if(evt->button == GDK_LEFT_BUTTON)
            {
                self->rubber_banding = TRUE;

                /* disable Gtk+ DnD callbacks, because else rubberbanding will be interrupted */
                gpointer drag_data = g_object_get_data(G_OBJECT(self),
                                        g_intern_static_string("gtk-site-data"));
                if(G_LIKELY(drag_data != NULL))
                {
                    g_signal_handlers_block_matched(G_OBJECT(self),
                                                    G_SIGNAL_MATCH_DATA, 0, 0,
                                                    NULL, NULL, drag_data);
                }

                /* FIXME: if you foward the event here, this will break rubber banding... */
                /* forward the event to root window */
                /* forward_event_to_rootwin(gtk_widget_get_screen(w), evt); */

                gtk_grab_add(w);
                self->rubber_banding_x = evt->x;
                self->rubber_banding_y = evt->y;
            }
        }
    }
    else if(evt->type == GDK_2BUTTON_PRESS)
    {
        if (evt->button == GDK_LEFT_BUTTON) /* left double click */
        {
            if (clicked_item)
            {
                clicked = FM_FV_ACTIVATED;
            }
            else
            {
                on_show_icons(NULL, NULL);
            }
        }
    }

    if(clicked != FM_FV_CLICK_NONE)
    {
        GtkTreeModel* model = GTK_TREE_MODEL(self->model);
        GtkTreePath* tp = NULL;

        if(clicked_item)
            tp = gtk_tree_model_get_path(model, &it);
        fm_folder_view_item_clicked(FM_FOLDER_VIEW(self), tp, clicked);
        if(tp)
            gtk_tree_path_free(tp);
    }
    /* forward the event to root window */
    else if(evt->button != GDK_LEFT_BUTTON)
        forward_event_to_rootwin(gtk_widget_get_screen(w), (GdkEvent*)evt);

    if(! gtk_widget_has_focus(w))
    {
        /* g_debug("we don't have the focus, grab it!"); */
        gtk_widget_grab_focus(w);
    }
    return TRUE;
}

static gboolean on_button_release(GtkWidget* w, GdkEventButton* evt)
{
    //g_print("on_button_release\n");

    FmDesktop* self = (FmDesktop*)w;
    GtkTreeIter it;
    FmDesktopItem* clicked_item = hit_test(self, &it, evt->x, evt->y);

    self->button_pressed = FALSE;

    if(self->rubber_banding)
    {
        /* re-enable Gtk+ DnD callbacks again */
        gpointer drag_data = g_object_get_data(G_OBJECT(self),
                                    g_intern_static_string("gtk-site-data"));
        if(G_LIKELY(drag_data != NULL))
        {
            g_signal_handlers_unblock_matched(G_OBJECT(self), G_SIGNAL_MATCH_DATA,
                                              0, 0, NULL, NULL, drag_data);
        }
        update_rubberbanding(self, evt->x, evt->y);
        gtk_grab_remove(w);
        self->rubber_banding = FALSE;
    }
    else if(self->dragging)
    {
        /*self->dragging = FALSE;*/
    }
    else if(fm_config->single_click && evt->button == 1)
    {
        if(clicked_item)
        {
            /* left single click */
            //fm_launch_file_simple(GTK_WINDOW(w), NULL, clicked_item->fi, pcmanfm_open_folder, w);
            fm_launch_file_simple(GTK_WINDOW(w), NULL, clicked_item->fi, NULL, w);
            return TRUE;
        }
    }

    /* forward the event to root window */
    if(! clicked_item)
        forward_event_to_rootwin(gtk_widget_get_screen(w), (GdkEvent*)evt);

    return TRUE;
}

static gboolean on_single_click_timeout(gpointer user_data)
{
    //g_print("on_single_click_timeout\n");

    FmDesktop* self = (FmDesktop*)user_data;
    GtkWidget* w = (GtkWidget*)self;
    GdkEventButton evt;
    GdkWindow* window;
    int x, y;

    window = gtk_widget_get_window(w);
    /* generate a fake button press */
    /* FIXME: will this cause any problem? */
    evt.type = GDK_BUTTON_PRESS;
    evt.window = window;
    gdk_window_get_pointer(window, &x, &y, &evt.state);
    evt.x = x;
    evt.y = y;
    evt.state |= GDK_BUTTON_PRESS_MASK;
    evt.state &= ~GDK_BUTTON_MOTION_MASK;
    on_button_press(GTK_WIDGET(self), &evt);
    evt.type = GDK_BUTTON_RELEASE;
    evt.state &= ~GDK_BUTTON_PRESS_MASK;
    evt.state |= ~GDK_BUTTON_RELEASE_MASK;
    on_button_release(GTK_WIDGET(self), &evt);

    self->single_click_timeout_handler = 0;
    return FALSE;
}

static gboolean on_motion_notify(GtkWidget* w, GdkEventMotion* evt)
{
    FmDesktop* self = (FmDesktop*)w;

    //g_print("self->button_pressed = %d, x = %d, y = %d\n", (int)self->button_pressed, (int)evt->x, (int)evt->y);

    if(! self->button_pressed)
    {
        self->dragging = FALSE;

        if(fm_config->single_click)
        {
            GtkTreeIter it;
            FmDesktopItem* item = hit_test(self, &it, evt->x, evt->y);
            GdkWindow* window = gtk_widget_get_window(w);

            if(item != self->hover_item)
            {
                if(0 != self->single_click_timeout_handler)
                {
                    g_source_remove(self->single_click_timeout_handler);
                    self->single_click_timeout_handler = 0;
                }
            }
            if(item)
            {
                if (!hand_cursor)
                    hand_cursor = gdk_cursor_new(GDK_HAND2);
                gdk_window_set_cursor(window, hand_cursor);
                /* FIXME: timeout should be customizable */
                if(self->single_click_timeout_handler == 0)
                    self->single_click_timeout_handler = g_timeout_add(400, on_single_click_timeout, self); //400 ms
                    /* Making a loop to aviod the selection of the item */
                    /* on_single_click_timeout(self); */
            }
            else
            {
                gdk_window_set_cursor(window, NULL);
            }
            self->hover_item = item;
        }
        return TRUE;
    }

    if(self->dragging)
    {
    }
    else if(self->rubber_banding)
    {
        update_rubberbanding(self, evt->x, evt->y);
    }
/*
    else
    {
        if (gtk_drag_check_threshold(w,
                                    self->drag_start_x,
                                    self->drag_start_y,
                                    evt->x, evt->y))
        {
            GtkTargetList* target_list;
            if(has_selected_item(self))
            {
                self->dragging = TRUE;
                target_list = gtk_drag_source_get_target_list(w);
                gtk_drag_begin(w, target_list,
                             GDK_ACTION_COPY|GDK_ACTION_MOVE|GDK_ACTION_LINK,
                             1, (GdkEvent*)evt);
            }
        }
    }
*/
    return TRUE;
}

static gboolean on_leave_notify(GtkWidget* w, GdkEventCrossing *evt)
{
    //g_print("on_leave_notify\n");

    FmDesktop* self = (FmDesktop*)w;
    if(self->single_click_timeout_handler)
    {
        g_source_remove(self->single_click_timeout_handler);
        self->single_click_timeout_handler = 0;
    }
    return TRUE;
}

static gboolean get_focused_item(FmDesktopItem* focus, GtkTreeModel* model, GtkTreeIter* it)
{
    FmDesktopItem* item;
    if(gtk_tree_model_get_iter_first(model, it)) do
    {
        item = fm_folder_model_get_item_userdata(FM_FOLDER_MODEL(model), it);
        CONTINUE_IF_ITEM_IS_NULL(item);
        if(item == focus)
            return item->is_selected;
    }
    while(gtk_tree_model_iter_next(model, it));
    return FALSE;
}

static gboolean on_key_press(GtkWidget* w, GdkEventKey* evt)
{
    FmDesktop* desktop = (FmDesktop*)w;
    FmDesktopItem* item;
    int modifier = (evt->state & gtk_accelerator_get_default_mod_mask());
    FmPathList* sels;
    GtkTreeModel* model;
    GtkTreePath* tp = NULL;
    GtkTreeIter it;
    switch (evt->keyval)
    {
    case GDK_KEY_Left:
        item = get_nearest_item(desktop, desktop->focus, GTK_DIR_LEFT);
        if(item)
        {
            if(0 == modifier)
            {
                _unselect_all(FM_FOLDER_VIEW(desktop));
                item->is_selected = TRUE;
            }
            set_focused_item(desktop, item);
        }
        return TRUE;
        break;
    case GDK_KEY_Right:
        item = get_nearest_item(desktop, desktop->focus, GTK_DIR_RIGHT);
        if(item)
        {
            if(0 == modifier)
            {
                _unselect_all(FM_FOLDER_VIEW(desktop));
                item->is_selected = TRUE;
            }
            set_focused_item(desktop, item);
        }
        return TRUE;
        break;
    case GDK_KEY_Up:
        item = get_nearest_item(desktop, desktop->focus, GTK_DIR_UP);
        if(item)
        {
            if(0 == modifier)
            {
                _unselect_all(FM_FOLDER_VIEW(desktop));
                item->is_selected = TRUE;
            }
            set_focused_item(desktop, item);
        }
        return TRUE;
        break;
    case GDK_KEY_Down:
        item = get_nearest_item(desktop, desktop->focus, GTK_DIR_DOWN);
        if(item)
        {
            if(0 == modifier)
            {
                _unselect_all(FM_FOLDER_VIEW(desktop));
                item->is_selected = TRUE;
            }
            set_focused_item(desktop, item);
        }
        return TRUE;
        break;
    case GDK_KEY_space:
        if(modifier & GDK_CONTROL_MASK)
        {
            if(desktop->focus)
            {
                desktop->focus->is_selected = !desktop->focus->is_selected;
                redraw_item(desktop, desktop->focus);
            }
            return TRUE;
        }
        break;
    case GDK_KEY_F2:
        sels = _dup_selected_file_paths(FM_FOLDER_VIEW(desktop));
        if(sels)
        {
            fm_rename_file(GTK_WINDOW(desktop), fm_path_list_peek_head(sels));
            fm_path_list_unref(sels);
        }
        break;
    case GDK_KEY_Return:
    case GDK_KEY_ISO_Enter:
    case GDK_KEY_KP_Enter:
        if(modifier == 0 && desktop->focus)
        {
            model = GTK_TREE_MODEL(desktop->model);
            if(get_focused_item(desktop->focus, model, &it))
            {
                tp = gtk_tree_model_get_path(model, &it);
                fm_folder_view_item_clicked(FM_FOLDER_VIEW(desktop), tp, FM_FV_ACTIVATED);
                if(tp)
                    gtk_tree_path_free(tp);
            }
        }
        break;
    }
    return GTK_WIDGET_CLASS(fm_desktop_parent_class)->key_press_event(w, evt);
}

static void on_style_set(GtkWidget* w, GtkStyle* prev)
{
    FmDesktop* self = (FmDesktop*)w;

    self->pango_timestamp++;

    PangoContext* pc = gtk_widget_get_pango_context(w);
    if (self->font_desc)
        pango_context_set_font_description(pc, self->font_desc);
    pango_layout_context_changed(self->pl);
}

static void on_direction_changed(GtkWidget* w, GtkTextDirection prev)
{
    FmDesktop* self = (FmDesktop*)w;
    pango_layout_context_changed(self->pl);
    queue_layout_items(self);
}

static void on_arrange_icons_rtl_changed(FmConfig* cfg, GtkWidget* w)
{
    FmDesktop * self = (FmDesktop *) w;
    queue_layout_items(self);
}

static void on_arrange_icons_btt_changed(FmConfig* cfg, GtkWidget* w)
{
    FmDesktop * self = (FmDesktop *) w;
    queue_layout_items(self);
}

static void on_arrange_icons_in_rows_changed(FmConfig* cfg, GtkWidget* w)
{
    FmDesktop * self = (FmDesktop *) w;
    queue_layout_items(self);
}

static void on_realize(GtkWidget* w)
{
    FmDesktop* self = (FmDesktop*)w;

    GTK_WIDGET_CLASS(fm_desktop_parent_class)->realize(w);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(w), TRUE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(w), TRUE);
    gtk_window_set_resizable((GtkWindow*)w, FALSE);

    wallpaper_manager_update_background(self, -1);
}

static gboolean on_focus_in(GtkWidget* w, GdkEventFocus* evt)
{
    FmDesktop* self = (FmDesktop*) w;
    GtkTreeIter it;
/*#if !GTK_CHECK_VERSION(2, 22, 0)
    GTK_WIDGET_SET_FLAGS(w, GTK_HAS_FOCUS);
#endif*/
    if(!self->focus && gtk_tree_model_get_iter_first(GTK_TREE_MODEL(self->model), &it))
        self->focus = fm_folder_model_get_item_userdata(self->model, &it);
    if(self->focus)
        redraw_item(self, self->focus);
    return  GTK_WIDGET_CLASS(fm_desktop_parent_class)->focus_in_event(w, evt);
}

static gboolean on_focus_out(GtkWidget* w, GdkEventFocus* evt)
{
    FmDesktop* self = (FmDesktop*) w;
    if(self->focus)
    {
/*#if !GTK_CHECK_VERSION(2, 22, 0)
        GTK_WIDGET_UNSET_FLAGS(w, GTK_HAS_FOCUS);
#endif*/
        redraw_item(self, self->focus);
    }
    return  GTK_WIDGET_CLASS(fm_desktop_parent_class)->focus_out_event(w, evt);
}

/* ---- Drag & Drop support ---- */

static void on_drag_begin(GtkWidget * widget, GdkDragContext * drag_context)
{
    //g_print("on_drag_begin\n");
    FmDesktop* desktop = FM_DESKTOP(widget);
    desktop->dragging = TRUE;
}

static void on_drag_end(GtkWidget * widget, GdkDragContext * drag_context)
{
    //g_print("on_drag_end\n");
    FmDesktop* desktop = FM_DESKTOP(widget);
    desktop->dragging = FALSE;
}
/*
static gboolean on_drag_failed(GtkWidget * widget, GdkDragContext * drag_context, GtkDragResult result)
{
    g_print("on_drag_failed\n");
    FmDesktop* desktop = FM_DESKTOP(widget);
    desktop->dragging = FALSE;
    return FALSE;
}
*/
static gboolean on_drag_motion (GtkWidget *dest_widget,
                                GdkDragContext *drag_context,
                                gint x, gint y, guint time)
{
    GdkAtom target;
    GdkDragAction action = 0;
    FmDesktop* desktop = FM_DESKTOP(dest_widget);
    FmDesktopItem* item;
    GtkTreeIter it;

    /*g_print("on_drag_motion, self->dragging = %d\n", (int)desktop->dragging);*/

    /* check if we're dragging over an item */
    item = hit_test(desktop, &it, x, y);

    if (desktop->dragging && item && item->is_selected)
        item = NULL;

    /* handle moving desktop items */
    if(!item)
    {
        if(fm_drag_context_has_target(drag_context, desktop_atom)
           && (gdk_drag_context_get_actions(drag_context) & GDK_ACTION_MOVE))
        {
            /* desktop item is being dragged */
            action = GDK_ACTION_MOVE; /* move desktop items */
            fm_dnd_dest_set_dest_file(desktop->dnd_dest, NULL);
        }
    }

    /* FmDndDest will do the rest */
    if(!action)
    {
        fm_dnd_dest_set_dest_file(desktop->dnd_dest,
                                  item ? item->fi : fm_folder_get_info(desktop_folder));
        target = gtk_drag_dest_find_target(dest_widget, drag_context, NULL);
        if(target != GDK_NONE &&
           fm_dnd_dest_is_target_supported(desktop->dnd_dest, target))
            action = fm_dnd_dest_get_default_action(desktop->dnd_dest, drag_context, target);
    }
    gdk_drag_status(drag_context, action, time);

    if(desktop->drop_hilight != item)
    {
        FmDesktopItem* old_drop = desktop->drop_hilight;
        if(action) /* don't hilight non-dropable item, see #3591767 */
            desktop->drop_hilight = item;
        if(old_drop)
            redraw_item(desktop, old_drop);
        if(item && action)
            redraw_item(desktop, item);
    }

    return (action != 0);
}

static void on_drag_leave (GtkWidget *dest_widget,
                           GdkDragContext *drag_context,
                           guint time)
{
    /*g_print("on_drag_leave\n");*/
    FmDesktop* desktop = FM_DESKTOP(dest_widget);

    if(desktop->drop_hilight)
    {
        FmDesktopItem* old_drop = desktop->drop_hilight;
        desktop->drop_hilight = NULL;
        redraw_item(desktop, old_drop);
    }
}

static gboolean on_drag_drop (GtkWidget *dest_widget,
                              GdkDragContext *drag_context,
                              gint x, gint y, guint time)
{
    /*g_print("on_drag_drop\n");*/

    FmDesktop* desktop = FM_DESKTOP(dest_widget);
    FmDesktopItem* item;
    GtkTreeIter it;

    /* check if we're dropping on an item */
    item = hit_test(desktop, &it, x, y);

    if (desktop->dragging && item && item->is_selected)
        item = NULL;

    /*g_print("item = %u\n", (unsigned)item);*/

    /* handle moving desktop items */
    if(!item)
    {
        if(fm_drag_context_has_target(drag_context, desktop_atom)
           && (gdk_drag_context_get_actions(drag_context) & GDK_ACTION_MOVE))
        {
            /* desktop item is being dragged */
            gtk_drag_get_data(dest_widget, drag_context, desktop_atom, time);
            return TRUE;
        }
    }
    return FALSE;
}

static void on_drag_data_received (GtkWidget *dest_widget,
                                   GdkDragContext *drag_context,
                                   gint x, gint y, GtkSelectionData *sel_data,
                                   guint info, guint time)
{
    /*g_print("on_drag_data_received\n");*/
    FmDesktop* desktop = FM_DESKTOP(dest_widget);
    GList *items, *l;
    int offset_x, offset_y;

    if(info != FM_DND_DEST_DESKTOP_ITEM)
    {
        //gtk_drag_finish(drag_context, FALSE, FALSE, time);
        return;
    }

    if (gdk_drag_context_get_selected_action(drag_context) != GDK_ACTION_MOVE)
    {
        //gtk_drag_finish(drag_context, FALSE, FALSE, time);
        return;
    }

    /* desktop items are being dragged */
    items = get_selected_items(desktop, NULL);
    offset_x = x - desktop->drag_start_x;
    offset_y = y - desktop->drag_start_y;
    for(l = items; l; l=l->next)
    {
        FmDesktopItem* item = (FmDesktopItem*)l->data;
        move_item(desktop, item, item->x + offset_x, item->y + offset_y, FALSE);
    }
    g_list_free(items);

    /* FIXME: save position of desktop icons everytime is
     * extremely inefficient, but currently inevitable. */
    save_item_pos(desktop);

    queue_layout_items(desktop);

    drag_context->action = GDK_ACTION_PRIVATE;
    gtk_drag_finish(drag_context, TRUE, FALSE, time);
}

static void on_dnd_src_data_get(FmDndSrc* ds, FmDesktop* desktop)
{
    /*g_print("on_dnd_src_data_get\n");*/
    FmFileInfoList* files = _dup_selected_files(FM_FOLDER_VIEW(desktop));
    if(files)
    {
        fm_dnd_src_set_files(ds, files);
        fm_file_info_list_unref(files);
    }
}

/****************************************************************************/

static void on_icon_theme_changed(GtkIconTheme* theme, FmDesktop* desktop)
{
    gtk_widget_queue_resize(GTK_WIDGET(desktop));
}

/****************************************************************************/

static void on_desktop_font_changed(FmConfig* cfg, FmDesktop* desktop)
{
    desktop->pango_timestamp++;

    if (desktop->font_desc)
        pango_font_description_free(desktop->font_desc);

    desktop->font_desc = NULL;

    if (app_config->desktop_font)
    {
        desktop->font_desc = pango_font_description_from_string(app_config->desktop_font);
        if (desktop->font_desc)
        {
                PangoContext* pc = gtk_widget_get_pango_context(GTK_WIDGET(desktop));
                pango_context_set_font_description(pc, desktop->font_desc);
                pango_layout_context_changed(desktop->pl);
                gtk_widget_queue_resize(GTK_WIDGET(desktop));
        }
    }

    queue_layout_items(desktop);
}

static void on_desktop_text_changed(FmConfig* cfg, FmDesktop* desktop)
{
    gtk_widget_queue_draw(GTK_WIDGET(desktop));
}

static void on_show_icons_changed(FmConfig* cfg, FmDesktop* desktop)
{
    if (desktop->popup_act_grp)
    {
        GtkAction * action = gtk_action_group_get_action(desktop->popup_act_grp, "ShowIcons");
        gtk_toggle_action_set_active(GTK_TOGGLE_ACTION(action), app_config->show_icons);
    }

    start_transition(desktop);
    gtk_widget_queue_draw(GTK_WIDGET(desktop));
}

static void on_desktop_icon_size_changed(FmConfig* cfg, FmDesktop* desktop)
{
    if (desktop->model)
    {
        fm_folder_model_set_icon_size(desktop->model, app_config->desktop_icon_size);
        gtk_widget_queue_resize(GTK_WIDGET(desktop));
        desktop->pango_timestamp++;
        queue_layout_items(desktop);
    }
}

static void on_overlap_state_changed(FmConfig* cfg, FmDesktop* desktop)
{
    queue_layout_items(desktop);
}

/****************************************************************************/

static void on_folder_start_loading(FmFolder* folder, FmDesktop* desktop)
{
    /* FIXME: should we delete the model here? */
}

static void on_folder_finish_loading(FmFolder* folder, FmDesktop* desktop)
{
    /* FIXME: we need to free old positions first?? */

    unload_items(desktop);
    load_items(desktop);
    start_transition(desktop);
}

static FmJobErrorAction on_folder_error(FmFolder* folder, GError* err, FmJobErrorSeverity severity, FmDesktop* desktop)
{
    if(err->domain == G_IO_ERROR)
    {
        if(err->code == G_IO_ERROR_NOT_MOUNTED && severity < FM_JOB_ERROR_CRITICAL)
        {
            FmPath* path = fm_folder_get_path(folder);
            if(fm_mount_path(NULL, path, TRUE))
                return FM_JOB_RETRY;
        }
    }
    fm_show_error(NULL, NULL, err->message);
    return FM_JOB_CONTINUE;
}

/****************************************************************************/

/* ---------------------------------------------------------------------
    FmDesktop class main handlers */

#if 0
static void on_desktop_model_destroy(gpointer data, GObject* model)
{
    g_signal_handlers_disconnect_by_func(app_config, on_desktop_icon_size_changed, model);
    *(gpointer*)data = NULL;
}
#endif

static inline void connect_model(FmDesktop* desktop)
{
    /* FIXME: different screens should be able to use different models */
    desktop->model = fm_folder_model_new(desktop_folder, FALSE);
    fm_folder_model_set_icon_size(desktop->model, app_config->desktop_icon_size);
    g_signal_connect(desktop->model, "row-deleting", G_CALLBACK(on_row_deleting), desktop);
    g_signal_connect(desktop->model, "row-inserted", G_CALLBACK(on_row_inserted), desktop);
    g_signal_connect(desktop->model, "row-deleted", G_CALLBACK(on_row_deleted), desktop);
    g_signal_connect(desktop->model, "row-changed", G_CALLBACK(on_row_changed), desktop);
    g_signal_connect(desktop->model, "rows-reordered", G_CALLBACK(on_rows_reordered), desktop);
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(desktop->model),
                                         app_config->desktop_sort_by,
                                         app_config->desktop_sort_type);

    g_signal_connect(desktop_folder, "start-loading", G_CALLBACK(on_folder_start_loading), desktop);
    g_signal_connect(desktop_folder, "finish-loading", G_CALLBACK(on_folder_finish_loading), desktop);
    g_signal_connect(desktop_folder, "error", G_CALLBACK(on_folder_error), desktop);

    fm_folder_reload(desktop_folder);
}

static inline void disconnect_model(FmDesktop* desktop)
{
    g_signal_handlers_disconnect_by_func(desktop_folder, on_folder_start_loading, desktop);
    g_signal_handlers_disconnect_by_func(desktop_folder, on_folder_finish_loading, desktop);
    g_signal_handlers_disconnect_by_func(desktop_folder, on_folder_error, desktop);

    g_signal_handlers_disconnect_by_func(desktop->model, on_row_deleting, desktop);
    g_signal_handlers_disconnect_by_func(desktop->model, on_row_inserted, desktop);
    g_signal_handlers_disconnect_by_func(desktop->model, on_row_deleted, desktop);
    g_signal_handlers_disconnect_by_func(desktop->model, on_row_changed, desktop);
    g_signal_handlers_disconnect_by_func(desktop->model, on_rows_reordered, desktop);
    g_object_unref(desktop->model);
    desktop->model = NULL;
}

#if GTK_CHECK_VERSION(3, 0, 0)
static void fm_desktop_destroy(GtkWidget *object)
#else
static void fm_desktop_destroy(GtkObject *object)
#endif
{
    FmDesktop *self;
    GdkScreen* screen;

    self = FM_DESKTOP(object);
    /* FIXME: what exactly this bug #3533958 is? */
    if(self->model) /* see bug #3533958 by korzhpavel@SF */
    {

        pango_font_description_free(self->font_desc);
        self->font_desc = NULL;

        screen = gtk_widget_get_screen((GtkWidget*)self);
        gdk_window_remove_filter(gdk_screen_get_root_window(screen), on_root_event, self);

        g_signal_handlers_disconnect_by_func(screen, on_screen_size_changed, self);
        g_signal_handlers_disconnect_by_func(screen, on_screen_monitors_changed, self);

        g_signal_handlers_disconnect_by_func(app_config, on_show_icons_changed, self);
        g_signal_handlers_disconnect_by_func(app_config, on_desktop_icon_size_changed, self);
        g_signal_handlers_disconnect_by_func(app_config, on_arrange_icons_rtl_changed, self);
        g_signal_handlers_disconnect_by_func(app_config, on_arrange_icons_btt_changed, self);
        g_signal_handlers_disconnect_by_func(app_config, on_arrange_icons_in_rows_changed, self);
        g_signal_handlers_disconnect_by_func(app_config, on_desktop_font_changed, self);
        g_signal_handlers_disconnect_by_func(app_config, on_desktop_text_changed, self);
        g_signal_handlers_disconnect_by_func(app_config, on_overlap_state_changed, self);

        g_signal_handlers_disconnect_by_func(gtk_icon_theme_get_default(), on_icon_theme_changed, self);

        gtk_window_group_remove_window(win_group, (GtkWindow*)self);

        disconnect_model(self);

        unload_items(self);

        g_object_unref(self->icon_render);
        g_object_unref(self->pl);

        /* FIXME: rename all IDs as <something>_handler_id */
        if(self->single_click_timeout_handler)
            g_source_remove(self->single_click_timeout_handler);

        if(self->transition_worker_handler_id)
            g_source_remove(self->transition_worker_handler_id);

        if(self->idle_layout)
            g_source_remove(self->idle_layout);

        g_signal_handlers_disconnect_by_func(self->dnd_src, on_dnd_src_data_get, self);
        g_object_unref(self->dnd_src);
        g_object_unref(self->dnd_dest);
    }

#if GTK_CHECK_VERSION(3, 0, 0)
    GTK_WIDGET_CLASS(fm_desktop_parent_class)->destroy(object);
#else
    GTK_OBJECT_CLASS(fm_desktop_parent_class)->destroy(object);
#endif
}

static void fm_desktop_init(FmDesktop *self)
{
}

/* we should have a constructor to handle parameters */
static GObject* fm_desktop_constructor(GType type, guint n_construct_properties,
                                       GObjectConstructParam *construct_properties)
{
    GObject* object = G_OBJECT_CLASS(fm_desktop_parent_class)->constructor(type, n_construct_properties, construct_properties);
    FmDesktop* self = (FmDesktop*)object;
    GdkScreen* screen = gtk_widget_get_screen((GtkWidget*)self);
    GdkWindow* root;
    //PangoContext* pc;
    guint i;
    gint n;
    GdkRectangle geom;

    /* FIXME: currently disabled; not easy to implement now*/
    const int MS_IN_S = 1000;
    self->transition_worker_handler_id = 0;
    self->transition_update_interval = 0.05 * MS_IN_S;
    //self->show_icons_transition_interval = 1.0 * MS_IN_S;
    self->show_icons_transition_interval = 0.05 * MS_IN_S;
    self->show_icons_transition_current = 0;


    for(i = 0; i < n_construct_properties; i++)
        if(!strcmp(construct_properties[i].pspec->name, "monitor")
           && G_VALUE_HOLDS_INT(construct_properties[i].value))
            self->monitor = g_value_get_int(construct_properties[i].value);
    if(self->monitor < 0)
        return object; /* this monitor is disabled */
    g_debug("fm_desktop_constructor for monitor %d", self->monitor);
    gdk_screen_get_monitor_geometry(screen, self->monitor, &geom);
    gtk_window_set_default_size((GtkWindow*)self, geom.width, geom.height);
    gtk_window_move(GTK_WINDOW(self), geom.x, geom.y);
    gtk_widget_set_app_paintable((GtkWidget*)self, TRUE);
    gtk_window_set_type_hint(GTK_WINDOW(self), GDK_WINDOW_TYPE_HINT_DESKTOP);
    gtk_widget_add_events((GtkWidget*)self,
                        GDK_POINTER_MOTION_MASK |
                        GDK_BUTTON_PRESS_MASK |
                        GDK_BUTTON_RELEASE_MASK |
                        GDK_KEY_PRESS_MASK|
                        GDK_PROPERTY_CHANGE_MASK);

    self->icon_render = fm_cell_renderer_pixbuf_new();
    g_object_set(self->icon_render, "follow-state", TRUE, NULL);
    g_object_ref_sink(self->icon_render);
    fm_cell_renderer_pixbuf_set_fixed_size(FM_CELL_RENDERER_PIXBUF(self->icon_render), app_config->desktop_icon_size, app_config->desktop_icon_size);

    /* FIXME: call pango_layout_context_changed() on the layout in response to the
     * "style-set" and "direction-changed" signals for the widget. */
    //pc = gtk_widget_get_pango_context((GtkWidget*)self);
    self->pl = gtk_widget_create_pango_layout((GtkWidget*)self, NULL);
    pango_layout_set_alignment(self->pl, PANGO_ALIGN_CENTER);
    pango_layout_set_ellipsize(self->pl, PANGO_ELLIPSIZE_END);
    pango_layout_set_wrap(self->pl, PANGO_WRAP_WORD_CHAR);

    self->pango_timestamp = 1;

    root = gdk_screen_get_root_window(screen);
    gdk_window_set_events(root, gdk_window_get_events(root)|GDK_PROPERTY_CHANGE_MASK);
    gdk_window_add_filter(root, on_root_event, self);
    g_signal_connect(screen, "size-changed", G_CALLBACK(on_screen_size_changed), self);
    g_signal_connect(screen, "monitors-changed", G_CALLBACK(on_screen_monitors_changed), self);

    n = get_desktop_for_root_window(root);
    if(n < 0)
        n = 0;
    self->cur_desktop = (guint)n;

    /* init dnd support */
    self->dnd_src = fm_dnd_src_new((GtkWidget*)self);

    /* add our own targets */
    /*
        We should not use fm_dnd_src_add_targets(), as it _prepends_ (not append) new targets to the target list.
    */
    //fm_dnd_src_add_targets((GtkWidget*)self, dnd_targets, G_N_ELEMENTS(dnd_targets));
    gtk_target_list_add(gtk_drag_source_get_target_list(GTK_WIDGET(self)),
        gdk_atom_intern_static_string(dnd_targets[0].target), dnd_targets[0].flags, dnd_targets[0].info);
    g_signal_connect(self->dnd_src, "data-get", G_CALLBACK(on_dnd_src_data_get), self);

    self->dnd_dest = fm_dnd_dest_new_with_handlers((GtkWidget*)self);
    //fm_dnd_dest_add_targets((GtkWidget*)self, dnd_targets, G_N_ELEMENTS(dnd_targets));
    gtk_target_list_add(gtk_drag_dest_get_target_list(GTK_WIDGET(self)),
        gdk_atom_intern_static_string(dnd_targets[0].target), dnd_targets[0].flags, dnd_targets[0].info);

    gtk_window_group_add_window(win_group, GTK_WINDOW(self));

    connect_model(self);
    load_items(self);

    fm_folder_view_add_popup(FM_FOLDER_VIEW(self), GTK_WINDOW(self),
                             fm_desktop_update_popup);


    g_signal_connect(app_config, "changed::show_icons", G_CALLBACK(on_show_icons_changed), self);
    g_signal_connect(app_config, "changed::desktop_icon_size", G_CALLBACK(on_desktop_icon_size_changed), self);
    g_signal_connect(app_config, "changed::arrange_icons_rtl", G_CALLBACK(on_arrange_icons_rtl_changed), self);
    g_signal_connect(app_config, "changed::arrange_icons_btt", G_CALLBACK(on_arrange_icons_btt_changed), self);
    g_signal_connect(app_config, "changed::arrange_icons_in_rows", G_CALLBACK(on_arrange_icons_in_rows_changed), self);
    g_signal_connect(app_config, "changed::desktop_font", G_CALLBACK(on_desktop_font_changed), self);
    g_signal_connect(app_config, "changed::desktop_text", G_CALLBACK(on_desktop_text_changed), self);
    g_signal_connect(app_config, "changed::overlap_state", G_CALLBACK(on_overlap_state_changed), self);

    g_signal_connect(gtk_icon_theme_get_default(), "changed", G_CALLBACK(on_icon_theme_changed), self);

    on_desktop_font_changed(NULL, self);

    return object;
}

FmDesktop *fm_desktop_new(GdkScreen* screen, gint monitor)
{
    g_debug("creating FmDesktop on screen %d, monitor %d", gdk_screen_get_number(screen), monitor);
    return g_object_new(FM_TYPE_DESKTOP, "screen", screen, "monitor", monitor, NULL);
}

static void fm_desktop_set_property(GObject *object, guint property_id,
                                    const GValue *value, GParamSpec *pspec)
{
    switch(property_id)
    {
        case PROP_MONITOR:
            FM_DESKTOP(object)->monitor = g_value_get_int(value);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
    }
}

static void fm_desktop_get_property(GObject *object, guint property_id,
                                    GValue *value, GParamSpec *pspec)
{
    switch(property_id)
    {
        case PROP_MONITOR:
            g_value_set_int(value, FM_DESKTOP(object)->monitor);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
    }
}

/* init for FmDesktop class */
static void fm_desktop_class_init(FmDesktopClass *klass)
{
    GtkWidgetClass* widget_class = GTK_WIDGET_CLASS(klass);
    typedef gboolean (*DeleteEvtHandler) (GtkWidget*, GdkEventAny*);
    char* atom_names[] = {"_NET_WORKAREA", "_NET_NUMBER_OF_DESKTOPS",
                          "_NET_CURRENT_DESKTOP", "_XROOTMAP_ID", "_XROOTPMAP_ID"};
    Atom atoms[G_N_ELEMENTS(atom_names)] = {0};
    GObjectClass* object_class = G_OBJECT_CLASS(klass);

#if GTK_CHECK_VERSION(3, 0, 0)
    widget_class->destroy = fm_desktop_destroy;
    widget_class->draw = on_draw;
    widget_class->get_preferred_width = on_get_preferred_width;
    widget_class->get_preferred_height = on_get_preferred_height;
#else
    GtkObjectClass *gtk_object_class = GTK_OBJECT_CLASS(klass);
    gtk_object_class->destroy = fm_desktop_destroy;

    widget_class->expose_event = on_expose;
    widget_class->size_request = on_size_request;
#endif
    widget_class->size_allocate = on_size_allocate;
    widget_class->button_press_event = on_button_press;
    widget_class->button_release_event = on_button_release;
    widget_class->motion_notify_event = on_motion_notify;
    widget_class->leave_notify_event = on_leave_notify;
    widget_class->key_press_event = on_key_press;
    widget_class->style_set = on_style_set;
    widget_class->direction_changed = on_direction_changed;
    widget_class->realize = on_realize;
    widget_class->focus_in_event = on_focus_in;
    widget_class->focus_out_event = on_focus_out;
    /* widget_class->scroll_event = on_scroll; */
    widget_class->delete_event = (DeleteEvtHandler)gtk_true;

    widget_class->drag_motion = on_drag_motion;
    widget_class->drag_drop = on_drag_drop;
    widget_class->drag_data_received = on_drag_data_received;
    widget_class->drag_leave = on_drag_leave;
    /* widget_class->drag_data_get = on_drag_data_get; */

    widget_class->drag_begin = on_drag_begin;
    widget_class->drag_end = on_drag_end;

    if(XInternAtoms(gdk_x11_get_default_xdisplay(), atom_names,
                    G_N_ELEMENTS(atom_names), False, atoms))
    {
        XA_NET_WORKAREA = atoms[0];
        XA_NET_NUMBER_OF_DESKTOPS = atoms[1];
        XA_NET_CURRENT_DESKTOP = atoms[2];
        XA_XROOTMAP_ID = atoms[3];
        XA_XROOTPMAP_ID = atoms[4];
    }

    object_class->constructor = fm_desktop_constructor;
    object_class->set_property = fm_desktop_set_property;
    object_class->get_property = fm_desktop_get_property;

    g_object_class_install_property(object_class, PROP_MONITOR,
        g_param_spec_int("monitor", "Monitor",
                         "Monitor number where desktop is",
                         0, 127, 0, G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE));

    desktop_atom = gdk_atom_intern_static_string(dnd_targets[0].target);
}


//static void on_clicked(FmFolderView* fv, FmFolderViewClickType type, FmFileInfo* file)

//static void on_sel_changed(FmFolderView* fv, FmFileInfoList* sels)

//static void on_sort_changed(FmFolderView* fv)

/* ---------------------------------------------------------------------
    FmFolderView interface implementation */

static void _set_sel_mode(FmFolderView* fv, GtkSelectionMode mode)
{
    /* not implemented */
}

static GtkSelectionMode _get_sel_mode(FmFolderView* fv)
{
    return GTK_SELECTION_MULTIPLE;
}

static void _set_sort(FmFolderView* fv, GtkSortType type, FmFolderModelViewCol by)
{
    if(type == (GtkSortType)app_config->desktop_sort_type &&
       by == (FmFolderModelViewCol)app_config->desktop_sort_by)
        return;
    app_config->desktop_sort_type = type;
    app_config->desktop_sort_by = type;
    pcmanfm_save_config(FALSE);
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(FM_DESKTOP(fv)->model),
                                         by, type);
}

static void _get_sort(FmFolderView* fv, GtkSortType* type, FmFolderModelViewCol* by)
{
    if(type)
        *type = app_config->desktop_sort_type;
    if(by)
        *by = app_config->desktop_sort_by;
}

static void _set_show_hidden(FmFolderView* fv, gboolean show)
{
    /* not implemented */
}

static gboolean _get_show_hidden(FmFolderView* fv)
{
    return FALSE;
}

static FmFolder* _get_folder(FmFolderView* fv)
{
    return desktop_folder;
}

static void _set_model(FmFolderView* fv, FmFolderModel* model)
{
    /* not implemented */
}

static FmFolderModel* _get_model(FmFolderView* fv)
{
    return FM_DESKTOP(fv)->model;
}

static gint _count_selected_files(FmFolderView* fv)
{
    FmDesktop* desktop = FM_DESKTOP(fv);
    GtkTreeModel* model = GTK_TREE_MODEL(desktop->model);
    GtkTreeIter it;
    gint n = 0;
    if(!gtk_tree_model_get_iter_first(model, &it))
        return 0;
    do
    {
        FmDesktopItem* item = fm_folder_model_get_item_userdata(desktop->model, &it);
        CONTINUE_IF_ITEM_IS_NULL(item);
        if(item->is_selected)
            n++;
    }
    while(gtk_tree_model_iter_next(model, &it));
    return n;
}

static FmFileInfoList* _dup_selected_files(FmFolderView* fv)
{
    /*g_print("_dup_selected_files\n");*/
    FmDesktop* desktop = FM_DESKTOP(fv);
    FmFileInfoList* files = NULL;
    GtkTreeModel* model = GTK_TREE_MODEL(desktop->model);
    GtkTreeIter it;
    if(!gtk_tree_model_get_iter_first(model, &it))
        return NULL;
    do
    {
        FmDesktopItem* item = fm_folder_model_get_item_userdata(desktop->model, &it);
        CONTINUE_IF_ITEM_IS_NULL(item);
        if(item->is_selected)
        {
            if(!files)
                files = fm_file_info_list_new();
            fm_file_info_list_push_tail(files, item->fi);
        }
    }
    while(gtk_tree_model_iter_next(model, &it));
    return files;
}

static FmPathList* _dup_selected_file_paths(FmFolderView* fv)
{
    FmDesktop* desktop = FM_DESKTOP(fv);
    FmPathList* files = NULL;
    GtkTreeModel* model = GTK_TREE_MODEL(desktop->model);
    GtkTreeIter it;
    if(!gtk_tree_model_get_iter_first(model, &it))
        return NULL;
    do
    {
        FmDesktopItem* item = fm_folder_model_get_item_userdata(desktop->model, &it);
        CONTINUE_IF_ITEM_IS_NULL(item);
        if(item->is_selected)
        {
            if(!files)
                files = fm_path_list_new();
            fm_path_list_push_tail(files, fm_file_info_get_path(item->fi));
        }
    }
    while(gtk_tree_model_iter_next(model, &it));
    return files;
}

typedef enum
{
    SEL_ACTION_SELECT,
    SEL_ACTION_UNSELECT,
    SEL_ACTION_INVERT
} select_action_t;

static void _select_all_with_action(FmFolderView* fv, select_action_t sel_action)
{
    FmDesktop* desktop = FM_DESKTOP(fv);
    GtkTreeIter it;
    GtkTreeModel* model = GTK_TREE_MODEL(desktop->model);
    if(!gtk_tree_model_get_iter_first(model, &it))
        return;
    do
    {
        FmDesktopItem* item = fm_folder_model_get_item_userdata(desktop->model, &it);
        CONTINUE_IF_ITEM_IS_NULL(item);
        gboolean is_selected;
        switch (sel_action){
            case SEL_ACTION_SELECT:
                is_selected = TRUE;
                break;
            case SEL_ACTION_UNSELECT:
                is_selected = FALSE;
                break;
            case SEL_ACTION_INVERT:
                is_selected = !item->is_selected;
                break;
        }
        if(item->is_selected != is_selected)
        {
            item->is_selected = is_selected;
            redraw_item(desktop, item);
        }
    }
    while(gtk_tree_model_iter_next(model, &it));
}

static void _select_all(FmFolderView* fv)
{
    _select_all_with_action(fv, SEL_ACTION_SELECT);
}

static void _unselect_all(FmFolderView* fv)
{
    _select_all_with_action(fv, SEL_ACTION_UNSELECT);
}

static void _select_invert(FmFolderView* fv)
{
    _select_all_with_action(fv, SEL_ACTION_INVERT);
}

static void _select_file_path(FmFolderView* fv, FmPath* path)
{
    /* not implemented */
}

static void _get_custom_menu_callbacks(FmFolderView* fv,
                                       FmFolderViewUpdatePopup* popup,
                                       FmLaunchFolderFunc* launch)
{
    if(popup)
        *popup = fm_desktop_update_item_popup;
/*    if(launch)
        *launch = pcmanfm_open_folder;*/
}

/* init for FmFolderView interface implementation */
static void fm_desktop_view_init(FmFolderViewInterface* iface)
{
    iface->set_sel_mode = _set_sel_mode;
    iface->get_sel_mode = _get_sel_mode;
    iface->set_sort = _set_sort;
    iface->get_sort = _get_sort;
    iface->set_show_hidden = _set_show_hidden;
    iface->get_show_hidden = _get_show_hidden;
    iface->get_folder = _get_folder;
    iface->set_model = _set_model;
    iface->get_model = _get_model;
    iface->count_selected_files = _count_selected_files;
    iface->dup_selected_files = _dup_selected_files;
    iface->dup_selected_file_paths = _dup_selected_file_paths;
    iface->select_all = _select_all;
    iface->unselect_all = _unselect_all;
    iface->select_invert = _select_invert;
    iface->select_file_path = _select_file_path;
    iface->get_custom_menu_callbacks = _get_custom_menu_callbacks;
}


