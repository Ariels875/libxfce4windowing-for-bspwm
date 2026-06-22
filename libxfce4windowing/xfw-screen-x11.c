/*
 * Copyright (c) 2022 Brian Tarricone <brian@tarricone.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301 USA
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <X11/X.h>
#include <gdk/gdkx.h>
#include <gio/gunixinputstream.h>  /* FIX #1: necesario para g_unix_input_stream_get_fd() */
#include <X11/Xatom.h>   // Para XA_WINDOW
#include <libwnck/libwnck.h>

#include "libxfce4windowing-private.h"
#include "xfw-monitor-x11.h"
#include "xfw-screen-private.h"
#include "xfw-screen-x11.h"
#include "xfw-util.h"
#include "xfw-window-x11.h"
#include "xfw-window-x11-bspwm.h"
#include "xfw-workspace-manager-x11.h"
#include "xfw-window-private.h"

struct _XfwScreenX11 {
    XfwScreen parent;

    WnckScreen *wnck_screen;
    GList *windows;
    GList *windows_stacked;
    GHashTable *wnck_windows;
    GHashTable *xid_windows;  /* Maps XID -> XfwWindowX11 for bspwm events */

    /* _NET_WORKAREA is defined for each workspace */
    GArray *workareas;  /* GdkRectangle */

    XfwMonitorManagerX11 *monitor_manager;

    gboolean is_bspwm;

    /* bspwm event subscription */
    GSubprocess *bspc_subscribe;
    GIOChannel  *bspc_stdout;
    guint        bspc_watch_id;
};

/* Forward declarations — todas las funciones estáticas que se referencian
 * antes de su definición deben declararse aquí. */
static void xfw_screen_x11_constructed(GObject *obj);
static void xfw_screen_x11_finalize(GObject *obj);
static GList *xfw_screen_x11_get_windows(XfwScreen *screen);
static GList *xfw_screen_x11_get_windows_stacked(XfwScreen *screen);
static void xfw_screen_x11_set_show_desktop(XfwScreen *screen, gboolean show);

static void window_opened(WnckScreen *wnck_screen, WnckWindow *window, XfwScreenX11 *screen);
static void window_closed(WnckScreen *wnck_screen, WnckWindow *window, XfwScreenX11 *screen);
static void active_window_changed(WnckScreen *wnck_screen, WnckWindow *previous_window, XfwScreenX11 *screen);
static void window_stacking_changed(WnckScreen *wnck_screen, XfwScreenX11 *screen);
static void showing_desktop_changed(WnckScreen *wnck_screen, XfwScreenX11 *screen);
static void window_manager_changed(WnckScreen *wnck_screen, XfwScreenX11 *screen);
static void active_workspace_changed(WnckScreen *wnck_screen, WnckWorkspace *previous_workspace, XfwScreenX11 *screen);

/* FIX #1 (forward declarations de las funciones bspwm que se usan en
 * xfw_screen_x11_constructed antes de estar definidas): */
static void xfw_screen_x11_start_bspc_subscription(XfwScreenX11 *screen);
static void xfw_screen_x11_stop_bspc_subscription(XfwScreenX11 *screen);
static gboolean xfw_screen_x11_bspc_event_callback(GIOChannel *channel,
                                                    GIOCondition condition,
                                                    XfwScreenX11 *screen);


G_DEFINE_FINAL_TYPE(XfwScreenX11, xfw_screen_x11, XFW_TYPE_SCREEN)


static void
xfw_screen_x11_class_init(XfwScreenX11Class *klass) {
    GObjectClass *gklass = G_OBJECT_CLASS(klass);
    gklass->constructed = xfw_screen_x11_constructed;
    gklass->finalize = xfw_screen_x11_finalize;

    XfwScreenClass *screen_class = XFW_SCREEN_CLASS(klass);
    screen_class->get_windows = xfw_screen_x11_get_windows;
    screen_class->get_windows_stacked = xfw_screen_x11_get_windows_stacked;
    screen_class->set_show_desktop = xfw_screen_x11_set_show_desktop;
}

static void
xfw_screen_x11_init(XfwScreenX11 *screen) {}

static gboolean
xfw_screen_x11_detect_bspwm(XfwScreenX11 *xscreen) {
    Display *display = GDK_DISPLAY_XDISPLAY(gdk_screen_get_display(_xfw_screen_get_gdk_screen(XFW_SCREEN(xscreen))));
    Window root = gdk_x11_screen_get_root_window(_xfw_screen_get_gdk_screen(XFW_SCREEN(xscreen)));

    Atom supporting_wm_check = XInternAtom(display, "_NET_SUPPORTING_WM_CHECK", False);
    Atom actual_type;
    int actual_format;
    unsigned long n_items, bytes_after;
    unsigned char *prop_data = NULL;

    /* Get _NET_SUPPORTING_WM_CHECK from root window */
    if (XGetWindowProperty(display, root, supporting_wm_check, 0, 1, False, XA_WINDOW,
                           &actual_type, &actual_format, &n_items, &bytes_after, &prop_data) == Success
        && prop_data != NULL && n_items > 0) {
        Window wm_window = *(Window *)prop_data;
        XFree(prop_data);
        prop_data = NULL;

        /* Get _NET_WM_NAME from the WM window */
        Atom net_wm_name = XInternAtom(display, "_NET_WM_NAME", False);
        Atom utf8_string = XInternAtom(display, "UTF8_STRING", False);

        if (XGetWindowProperty(display, wm_window, net_wm_name, 0, 1024, False, utf8_string,
                               &actual_type, &actual_format, &n_items, &bytes_after, &prop_data) == Success
            && prop_data != NULL && n_items > 0) {
            gchar *wm_name = g_strndup((gchar *)prop_data, n_items);
            XFree(prop_data);

            gboolean is_bspwm = (g_strcmp0(wm_name, "bspwm") == 0);
            g_free(wm_name);
            return is_bspwm;
        }
    }

    if (prop_data != NULL) {
        XFree(prop_data);
    }

    return FALSE;
}

static void
xfw_screen_x11_constructed(GObject *obj) {
    XfwScreen *screen = XFW_SCREEN(obj);
    XfwScreenX11 *xscreen = XFW_SCREEN_X11(obj);

    G_OBJECT_CLASS(xfw_screen_x11_parent_class)->constructed(obj);

    XfwSeat *default_seat = g_object_new(XFW_TYPE_SEAT,
                                         "name", "seat0",
                                         NULL);
    _xfw_screen_seat_added(screen, default_seat);

    _xfw_screen_set_workspace_manager(screen, _xfw_workspace_manager_x11_new(screen));

    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    xscreen->wnck_screen = g_object_ref(wnck_screen_get(gdk_x11_screen_get_screen_number(_xfw_screen_get_gdk_screen(screen))));
    G_GNUC_END_IGNORE_DEPRECATIONS
    xscreen->wnck_windows = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_object_unref);
    xscreen->xid_windows  = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, NULL);

    /* Detect if we're running under bspwm */
    xscreen->is_bspwm = xfw_screen_x11_detect_bspwm(xscreen);

    if (xscreen->is_bspwm) {
        /* Las forward declarations de arriba hacen que esta llamada sea válida
         * aunque la definición esté más abajo en el archivo. */
        xfw_screen_x11_start_bspc_subscription(xscreen);
    }

    for (GList *l = wnck_screen_get_windows(xscreen->wnck_screen); l != NULL; l = l->next) {
        GType window_type = xscreen->is_bspwm ? XFW_TYPE_WINDOW_X11_BSPWM : XFW_TYPE_WINDOW_X11;
        XfwWindowX11 *window = g_object_new(window_type,
                                            "screen", screen,
                                            "wnck-window", l->data,
                                            NULL);
        xscreen->windows = g_list_prepend(xscreen->windows, window);
        g_hash_table_insert(xscreen->wnck_windows, l->data, window);

        Window xid = wnck_window_get_xid(WNCK_WINDOW(l->data));
        if (xid != 0) {
            /* FIX #4: GSIZE_TO_POINTER en lugar de GUINT_TO_POINTER.
             * Window es typedef unsigned long (8 bytes en x86-64).
             * GUINT_TO_POINTER solo preserva 32 bits y trunca XIDs en 64-bit. */
            g_hash_table_insert(xscreen->xid_windows, GSIZE_TO_POINTER((gsize)xid), window);
        }
    }
    xscreen->windows = g_list_reverse(xscreen->windows);
    window_stacking_changed(xscreen->wnck_screen, xscreen);

    _xfw_screen_set_active_window(screen,
                                  g_hash_table_lookup(xscreen->wnck_windows,
                                                      wnck_screen_get_active_window(xscreen->wnck_screen)));

    g_signal_connect(xscreen->wnck_screen, "window-opened",           G_CALLBACK(window_opened),           xscreen);
    g_signal_connect(xscreen->wnck_screen, "window-closed",           G_CALLBACK(window_closed),           xscreen);
    g_signal_connect(xscreen->wnck_screen, "active-window-changed",   G_CALLBACK(active_window_changed),   xscreen);
    g_signal_connect(xscreen->wnck_screen, "window-stacking-changed", G_CALLBACK(window_stacking_changed), xscreen);
    g_signal_connect(xscreen->wnck_screen, "window-manager-changed",  G_CALLBACK(window_manager_changed),  xscreen);
    g_signal_connect(xscreen->wnck_screen, "showing-desktop-changed", G_CALLBACK(showing_desktop_changed), xscreen);
    g_signal_connect(xscreen->wnck_screen, "active-workspace-changed",G_CALLBACK(active_workspace_changed),xscreen);

    xscreen->monitor_manager = _xfw_monitor_manager_x11_new(xscreen);
}

/* ---------------------------------------------------------------------------
 * bspwm: suscripción a eventos de bspc subscribe
 * ---------------------------------------------------------------------------
 *
 * La arquitectura es:
 *   1. start_bspc_subscription  — lanza "bspc subscribe node_state node_flag"
 *                                  y conecta un GIOChannel a su stdout.
 *   2. bspc_event_callback      — lee cada línea, parsea el evento y actualiza
 *                                  el XfwWindowState del XfwWindowX11 afectado.
 *   3. stop_bspc_subscription   — mata el proceso y libera recursos.
 */

static void
xfw_screen_x11_start_bspc_subscription(XfwScreenX11 *screen) {
    GError *error = NULL;
    /* argv estático: g_subprocess_newv NO toma ownership, no hace falta heap */
    const gchar *argv[] = { "bspc", "subscribe", "node_state", "node_flag", NULL };

    /* FIX #1 (parte 1/2): eliminamos la variable envp que se obtenía con
     * g_get_environ() y se liberaba sin pasarse a nadie.
     * g_subprocess_newv hereda el entorno del proceso padre por defecto. */
    GSubprocess *subprocess = g_subprocess_newv(argv,
                                                G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                                G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                                &error);

    if (subprocess == NULL) {
        g_warning("xfw-screen-x11: failed to start bspc subscription: %s", error->message);
        g_error_free(error);
        return;
    }

    screen->bspc_subscribe = subprocess;

    /* FIX #1 (parte 2/2): g_subprocess_get_stdout_pipe() retorna GInputStream*,
     * NO un gint fd. Para obtener el fd Unix subyacente usamos
     * g_unix_input_stream_get_fd() sobre el GUnixInputStream que GSubprocess
     * crea internamente cuando se pide STDOUT_PIPE.
     *
     * IMPORTANTE: el fd sigue siendo propiedad de GSubprocess; NO llamar a
     * g_io_channel_set_close_on_unref(TRUE) porque causaría un doble cierre
     * al destruir el GSubprocess. */
    GInputStream *stdout_stream = g_subprocess_get_stdout_pipe(subprocess);
    if (stdout_stream != NULL) {
        gint stdout_fd = g_unix_input_stream_get_fd(G_UNIX_INPUT_STREAM(stdout_stream));
        screen->bspc_stdout = g_io_channel_unix_new(stdout_fd);
        g_io_channel_set_close_on_unref(screen->bspc_stdout, FALSE);
        g_io_channel_set_encoding(screen->bspc_stdout, NULL, NULL);
        g_io_channel_set_flags(screen->bspc_stdout, G_IO_FLAG_NONBLOCK, NULL);
        screen->bspc_watch_id = g_io_add_watch(screen->bspc_stdout,
                                               G_IO_IN | G_IO_HUP | G_IO_ERR,
                                               (GIOFunc)xfw_screen_x11_bspc_event_callback,
                                               screen);
    }
}

static void
xfw_screen_x11_stop_bspc_subscription(XfwScreenX11 *screen) {
    if (screen->bspc_watch_id > 0) {
        g_source_remove(screen->bspc_watch_id);
        screen->bspc_watch_id = 0;
    }
    if (screen->bspc_stdout != NULL) {
        g_io_channel_unref(screen->bspc_stdout);
        screen->bspc_stdout = NULL;
    }
    if (screen->bspc_subscribe != NULL) {
        g_subprocess_force_exit(screen->bspc_subscribe);
        g_object_unref(screen->bspc_subscribe);
        screen->bspc_subscribe = NULL;
    }
}

static gboolean
xfw_screen_x11_bspc_event_callback(GIOChannel *channel, GIOCondition condition, XfwScreenX11 *screen) {
    if (condition & (G_IO_HUP | G_IO_ERR)) {
        /* El pipe se cerró o hubo un error; reiniciamos la suscripción. */
        xfw_screen_x11_stop_bspc_subscription(screen);
        xfw_screen_x11_start_bspc_subscription(screen);
        return G_SOURCE_REMOVE;
    }

    if (condition & G_IO_IN) {
        gchar *line = NULL;
        gsize length = 0;
        GError *error = NULL;
        GIOStatus status = g_io_channel_read_line(channel, &line, &length, NULL, &error);

        if (status == G_IO_STATUS_NORMAL && line != NULL) {
            /* FIX #5: g_io_channel_read_line() incluye el terminador '\n' (o
             * '\r\n') en el string. Sin este g_strchomp(), parts[3] sería
             * "on\n" o "off\n" y g_strcmp0(parts[3], "on") siempre fallaría,
             * haciendo que todos los eventos bspc fueran ignorados. */
            g_strchomp(line);

            /* Formato del evento bspc subscribe:
             *   node_state <monitor_id> <desktop_id> <node_id> hidden on|off
             *   node_flag  <monitor_id> <desktop_id> <node_id> hidden on|off
             *
             * Nota: bspc emite los IDs en DECIMAL, no en hexadecimal.
             * El node_id en bspwm corresponde al Window XID de X11. */
            gchar **parts = g_strsplit(line, " ", -1);
            guint n_parts = g_strv_length(parts);

            /* FIX #4 (parsing): bspc subscribe emite 6 tokens:
             *   parts[0] = "node_state" | "node_flag"
             *   parts[1] = monitor_id   (decimal)
             *   parts[2] = desktop_id   (decimal)
             *   parts[3] = node_id/XID  (decimal)
             *   parts[4] = "hidden"
             *   parts[5] = "on" | "off"
             *
             * El agente original asumía 4 tokens y XID en parts[1] con
             * prefijo "0x". El formato real tiene 6 tokens y XID en parts[3]
             * en decimal. Usamos base 0 en strtoull para ser robustos ante
             * ambas representaciones (decimal y "0x" hex). */
            if (n_parts >= 6) {
                if (g_strcmp0(parts[0], "node_state") == 0 || g_strcmp0(parts[0], "node_flag") == 0) {
                    if (g_strcmp0(parts[4], "hidden") == 0) {
                        /* FIX #4: base=0 detecta automáticamente decimal vs
                         * 0x-prefixed hex; no asumimos ningún formato fijo. */
                        Window xid = (Window)g_ascii_strtoull(parts[3], NULL, 0);
                        gboolean hidden = (g_strcmp0(parts[5], "on") == 0);

                        /* FIX #4: GSIZE_TO_POINTER para coincidir con la
                         * clave usada al insertar en xid_windows. */
                        XfwWindowX11 *window = g_hash_table_lookup(screen->xid_windows,
                                                                    GSIZE_TO_POINTER((gsize)xid));
                        if (window != NULL) {
                            XfwWindowState old_state = _xfw_window_x11_get_state(window);
                            XfwWindowState new_state = old_state;

                            if (hidden) {
                                new_state |= XFW_WINDOW_STATE_MINIMIZED;
                            } else {
                                new_state &= ~XFW_WINDOW_STATE_MINIMIZED;
                            }

                            XfwWindowState changed_mask = old_state ^ new_state;
                            if (changed_mask != XFW_WINDOW_STATE_NONE) {
                                /* Actualizamos el estado a través del accessor
                                 * privado; no accedemos a ->priv directamente
                                 * desde este archivo (pertenece a xfw-window-x11.c). */
                                _xfw_window_x11_set_state(window, new_state);
                                g_signal_emit_by_name(window, "state-changed",
                                                      changed_mask, new_state);
                            }
                        }
                    }
                }
            }
            g_strfreev(parts);
            g_free(line);
        } else if (status == G_IO_STATUS_EOF) {
            /* El proceso bspc terminó; reiniciamos. */
            xfw_screen_x11_stop_bspc_subscription(screen);
            xfw_screen_x11_start_bspc_subscription(screen);
            return G_SOURCE_REMOVE;
        } else if (error != NULL) {
            g_warning("xfw-screen-x11: error reading bspc events: %s", error->message);
            g_error_free(error);
        }
    }

    return G_SOURCE_CONTINUE;
}

/* --------------------------------------------------------------------------- */

static void
xfw_screen_x11_finalize(GObject *obj) {
    XfwScreenX11 *screen = XFW_SCREEN_X11(obj);

    xfw_screen_x11_stop_bspc_subscription(screen);

    _xfw_monitor_manager_x11_destroy(screen->monitor_manager);

    g_signal_handlers_disconnect_by_data(screen->wnck_screen, screen);
    g_list_free(screen->windows);
    g_list_free(screen->windows_stacked);
    g_hash_table_destroy(screen->wnck_windows);
    g_hash_table_destroy(screen->xid_windows);

    if (screen->workareas != NULL) {
        g_array_free(screen->workareas, TRUE);
    }

    /* to be released last */
    g_object_unref(screen->wnck_screen);

    G_OBJECT_CLASS(xfw_screen_x11_parent_class)->finalize(obj);
}

static GList *
xfw_screen_x11_get_windows(XfwScreen *screen) {
    return XFW_SCREEN_X11(screen)->windows;
}

static GList *
xfw_screen_x11_get_windows_stacked(XfwScreen *screen) {
    return XFW_SCREEN_X11(screen)->windows_stacked;
}

static void
xfw_screen_x11_set_show_desktop(XfwScreen *screen, gboolean show) {
    XfwScreenX11 *xscreen = XFW_SCREEN_X11(screen);
    if (!!show != wnck_screen_get_showing_desktop(xscreen->wnck_screen)) {
        wnck_screen_toggle_showing_desktop(xscreen->wnck_screen, show);
        _xfw_screen_set_show_desktop(screen, !!show);
    }
}

static void
window_opened(WnckScreen *wnck_screen, WnckWindow *wnck_window, XfwScreenX11 *screen) {
    GType window_type = screen->is_bspwm ? XFW_TYPE_WINDOW_X11_BSPWM : XFW_TYPE_WINDOW_X11;
    XfwWindowX11 *window = XFW_WINDOW_X11(g_object_new(window_type,
                                                       "screen", screen,
                                                       "wnck-window", wnck_window,
                                                       NULL));
    screen->windows = g_list_prepend(screen->windows, window);
    g_hash_table_insert(screen->wnck_windows, wnck_window, window);

    Window xid = wnck_window_get_xid(wnck_window);
    if (xid != 0) {
        /* FIX #4: GSIZE_TO_POINTER */
        g_hash_table_insert(screen->xid_windows, GSIZE_TO_POINTER((gsize)xid), window);
    }

    /* FIXME: window-stacking-changed signal will fire out of order */
    window_stacking_changed(screen->wnck_screen, screen);
    g_signal_emit_by_name(screen, "window-opened", window);
}

static void
window_closed(WnckScreen *wnck_screen, WnckWindow *wnck_window, XfwScreenX11 *screen) {
    XfwWindowX11 *window = g_hash_table_lookup(screen->wnck_windows, wnck_window);
    if (window != NULL) {
        g_object_ref(window);

        Window xid = wnck_window_get_xid(wnck_window);
        if (xid != 0) {
            /* FIX #4: GSIZE_TO_POINTER */
            g_hash_table_remove(screen->xid_windows, GSIZE_TO_POINTER((gsize)xid));
        }

        g_hash_table_remove(screen->wnck_windows, wnck_window);
        screen->windows = g_list_remove(screen->windows, window);
        screen->windows_stacked = g_list_remove(screen->windows_stacked, window);

        if (xfw_screen_get_active_window(XFW_SCREEN(screen)) == XFW_WINDOW(window)) {
            _xfw_screen_set_active_window(XFW_SCREEN(screen), NULL);
        }

        g_signal_emit_by_name(window, "closed");
        g_signal_emit_by_name(screen, "window-closed", window);
        g_signal_emit_by_name(screen, "window-stacking-changed", screen);

        g_object_unref(window);
    }
}

static void
active_window_changed(WnckScreen *wnck_screen, WnckWindow *previous_wnck_window, XfwScreenX11 *screen) {
    WnckWindow *wnck_window = wnck_screen_get_active_window(screen->wnck_screen);
    XfwWindow *window = g_hash_table_lookup(screen->wnck_windows, wnck_window);

    if (window != xfw_screen_get_active_window(XFW_SCREEN(screen))) {
        /* FIX #6: el código original emitía "state-changed" directamente sobre
         * los WnckWindow (objetos de libwnck), pero el panel escucha señales
         * de XfwWindow (objetos de libxfce4windowing).  Buscamos el XfwWindow
         * correspondiente en el hash y notificamos sobre él.
         *
         * g_object_notify() dispara "notify::state" para que los consumidores
         * (como el plugin tasklist) puedan re-leer el estado activo/inactivo. */
        if (previous_wnck_window != NULL) {
            XfwWindow *previous_xfw_window = g_hash_table_lookup(screen->wnck_windows,
                                                                  previous_wnck_window);
            if (previous_xfw_window != NULL) {
                g_object_notify(G_OBJECT(previous_xfw_window), "state");
            }
        }
        if (wnck_window != NULL && window != NULL) {
            g_object_notify(G_OBJECT(window), "state");
        }

        _xfw_screen_set_active_window(XFW_SCREEN(screen), window);
    }
}

static void
window_stacking_changed(WnckScreen *wnck_screen, XfwScreenX11 *screen) {
    g_clear_list(&screen->windows_stacked, NULL);

    for (GList *l = wnck_screen_get_windows_stacked(screen->wnck_screen); l != NULL; l = l->next) {
        XfwWindowX11 *window = g_hash_table_lookup(screen->wnck_windows, l->data);
        if (window != NULL) {
            screen->windows_stacked = g_list_prepend(screen->windows_stacked, window);
        }
    }
    screen->windows_stacked = g_list_reverse(screen->windows_stacked);
    g_signal_emit_by_name(screen, "window-stacking-changed");
}

static void
window_manager_changed(WnckScreen *wnck_screen, XfwScreenX11 *screen) {
    g_signal_emit_by_name(screen, "window-manager-changed");
}

static void
showing_desktop_changed(WnckScreen *wnck_screen, XfwScreenX11 *screen) {
    gboolean show_desktop = wnck_screen_get_showing_desktop(wnck_screen);
    _xfw_screen_set_show_desktop(XFW_SCREEN(screen), show_desktop);
}

static void
active_workspace_changed(WnckScreen *wnck_screen, WnckWorkspace *previous_workspace, XfwScreenX11 *screen) {
    WnckWorkspace *cur_workspace = wnck_screen_get_active_workspace(screen->wnck_screen);
    gint cur_workspace_num = cur_workspace != NULL
                                 ? wnck_workspace_get_number(cur_workspace)
                                 : 0;
    _xfw_monitor_x11_workspace_changed(screen, cur_workspace_num);
}

XfwWorkspace *
_xfw_screen_x11_workspace_for_wnck_workspace(XfwScreenX11 *screen, WnckWorkspace *wnck_workspace) {
    XfwWorkspaceManager *workspace_manager = xfw_screen_get_workspace_manager(XFW_SCREEN(screen));
    return _xfw_workspace_manager_x11_workspace_for_wnck_workspace(XFW_WORKSPACE_MANAGER_X11(workspace_manager),
                                                                   wnck_workspace);
}

GArray *
_xfw_screen_x11_get_workareas(XfwScreenX11 *screen) {
    return screen->workareas;
}

void
_xfw_screen_x11_set_workareas(XfwScreenX11 *screen, GArray *workareas) {
    if (screen->workareas != NULL && screen->workareas != workareas) {
        g_array_free(screen->workareas, TRUE);
    }
    screen->workareas = workareas;
}

gboolean
_xfw_screen_x11_is_bspwm(XfwScreenX11 *screen) {
    return screen->is_bspwm;
}
