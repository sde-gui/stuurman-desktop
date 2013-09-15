/**
 * Copyright (c) 2013 Vadim Ushakov
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "window-tracker.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <sde-utils-x11.h>
#include <gdk/gdk.h>
#include <gdk/gdkx.h>

#include "pcmanfm.h"
#include "app-config.h"

static GdkFilterReturn _event_filter(GdkXEvent *xevent, GdkEvent *event, gpointer not_used);


typedef struct _window_t
{
    Window xid;
    GdkWindow * gdk_window;
    GdkRectangle rect;
    gboolean dont_overlap_desktop_icons : 1;
//    gboolean all_desktops : 1;
//    gboolean visible : 1;
    gboolean used : 1;
} window_t;

static GSList * window_list = NULL;


static guint emit_signal_idle_cb = 0;

static gboolean emit_signal_real(void)
{
    fm_config_emit_changed(fm_config, "overlap_state");
    emit_signal_idle_cb = 0;
    return FALSE;
}

static void emit_signal(void)
{
    if (!emit_signal_idle_cb)
        emit_signal_idle_cb = g_idle_add((GSourceFunc)emit_signal_real, NULL);
}

static window_t * window_from_xid(Window xid)
{
    GSList * l;
    for (l = window_list; l; l = l->next)
    {
        window_t * window = (window_t *) l->data;
        if (window-> xid == xid)
        {
            window->used = TRUE;
            return window;
        }
    }
    return NULL;
}

static void delete_unused_windows(void)
{
    GSList * l;
    GSList ** backlink = &window_list;
    for (l = window_list; l; l = l->next)
    {
again:
        if (!l)
            break;

        window_t * window = (window_t *) l->data;
        if (!window->used)
        {
            *backlink = l->next;

            if (window->dont_overlap_desktop_icons)
                emit_signal();

            if (window->gdk_window)
            {
                gdk_window_remove_filter(window->gdk_window, (GdkFilterFunc)_event_filter, NULL);
                gdk_window_unref(window->gdk_window);
            }

            g_free(window);
            g_slist_free_1(l);
            l = *backlink;
            goto again;
        }
        backlink = &l->next;
    }
}

static void mark_all_window_as_unused(void)
{
    GSList * l;
    for (l = window_list; l; l = l->next)
    {
        window_t * window = (window_t *) l->data;
        window->used = FALSE;
    }
}


static gboolean read_window_overlap_status(window_t * window)
{
    if (!window)
        return FALSE;

    int count = 0;
    guint32 *data = su_x11_get_xa_property(gdk_x11_get_default_xdisplay(), window->xid,
        a_SDE_DONT_OVERLAP_DESKTOP_ICONS, XA_CARDINAL, &count);

    gboolean overlap_value = FALSE;
    GdkRectangle tmp = {0, 0, 0, 0};

    if (data && count == 4)
    {
        tmp.x = data[0];
        tmp.y = data[1];
        tmp.width = data[2];
        tmp.height = data[3];
        overlap_value = TRUE;
    }
    else
    {
        overlap_value = FALSE;
    }

    if (data)
        XFree(data);

    if ((window->rect.x      == tmp.x)
    &&  (window->rect.y      == tmp.y)
    &&  (window->rect.width  == tmp.width)
    &&  (window->rect.height == tmp.height)
    &&  (window->dont_overlap_desktop_icons == overlap_value))
        return FALSE;

    window->rect.x      = tmp.x;
    window->rect.y      = tmp.y;
    window->rect.width  = tmp.width;
    window->rect.height = tmp.height;
    window->dont_overlap_desktop_icons = overlap_value;

    return TRUE;
}

static void create_window_for_xid(Window xid)
{
    window_t * window = g_new0(window_t, 1);
    window->xid = xid;
    window->used = TRUE;
    window_list = g_slist_prepend(window_list, window);


    XWindowAttributes window_attributes = {0,};
    XGetWindowAttributes(gdk_x11_get_default_xdisplay(), xid, &window_attributes);
    if (!(window_attributes.your_event_mask & PropertyChangeMask))
    {
        XSelectInput(gdk_x11_get_default_xdisplay(), xid, window_attributes.your_event_mask | PropertyChangeMask);
    }

    window->gdk_window = gdk_x11_window_foreign_new_for_display(gdk_display_get_default(), xid);
    if (window->gdk_window)
        gdk_window_add_filter(window->gdk_window, (GdkFilterFunc)_event_filter, NULL);

    if (read_window_overlap_status(window))
        emit_signal();
}

static void on_net_client_list()
{
    int client_count;
    Window * client_list = su_x11_get_xa_property(gdk_x11_get_default_xdisplay(),
        GDK_ROOT_WINDOW(), a_NET_CLIENT_LIST, XA_WINDOW, &client_count);

    if (!client_list)
        return;

    mark_all_window_as_unused();

    int i;
    for (i = 0; i < client_count; i++)
    {
        if (window_from_xid(client_list[i]) == NULL)
        {
            create_window_for_xid(client_list[i]);
        }
    }

    XFree(client_list);

    delete_unused_windows();
}

static GdkFilterReturn _event_filter(GdkXEvent *xevent, GdkEvent *event, gpointer not_used)
{
    XEvent *ev = (XEvent *) xevent;

    Window win = ev->xany.window;

    if (ev->type != PropertyNotify)
        return GDK_FILTER_CONTINUE;

    Atom at = ev->xproperty.atom;

    //g_print("PropertyNotify 0x%x %s\n", (int)win, gdk_x11_get_xatom_name(at));

    if (win == GDK_ROOT_WINDOW())
    {
        if (at == a_NET_CLIENT_LIST)
        {
            on_net_client_list();
        }
    }
    else if (at == a_SDE_DONT_OVERLAP_DESKTOP_ICONS)
    {
        window_t * window = window_from_xid(win);
        if (read_window_overlap_status(window))
            emit_signal();
    }

    return GDK_FILTER_CONTINUE;
}


void fm_window_tracker_initialize(void)
{
    XSelectInput(gdk_x11_get_default_xdisplay(), GDK_ROOT_WINDOW(), StructureNotifyMask|SubstructureNotifyMask|PropertyChangeMask);
    gdk_window_add_filter(NULL, (GdkFilterFunc)_event_filter, NULL);
    on_net_client_list();
}

void fm_window_tracker_finalize(void)
{
    gdk_window_remove_filter(NULL, (GdkFilterFunc)_event_filter, NULL);
}

gboolean fm_window_tracker_test_overlap(GdkRectangle * rect)
{

    GSList * l;
    for (l = window_list; l; l = l->next)
    {
        window_t * window = (window_t *) l->data;

        if (window->used && window->dont_overlap_desktop_icons)
        {
            if (gdk_rectangle_intersect(rect, &window->rect, NULL))
            {
/*
                g_debug("xid = %d: %d:%d-%d:%d overlaps %d:%d-%d:%d", (int)window->xid,
                    (int)window->rect.x, (int)window->rect.y,
                    (int)window->rect.width, (int)window->rect.height,
                    (int)rect->x, rect->y,
                    (int)rect->width, rect->height);
*/
                return TRUE;
            }
        }
    }

    return FALSE;
}

