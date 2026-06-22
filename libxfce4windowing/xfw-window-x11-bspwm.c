/*
 * Copyright (c) 2024 Brian Tarricone <brian@tarricone.org>
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

#include <libwnck/libwnck.h>
#include <gio/gio.h>

#include "libxfce4windowing-private.h"
#include "xfw-screen-x11.h"
#include "xfw-window-x11.h"
#include "xfw-window-x11-bspwm.h"
#include "xfw-window-private.h"
#include "xfw-wnck-icon.h"
#include "libxfce4windowing-visibility.h"

struct _XfwWindowX11BspwmPrivate {
    gboolean is_bspwm;
};

static void xfw_window_x11_bspwm_constructed(GObject *obj);
static gboolean xfw_window_x11_bspwm_set_minimized(XfwWindow *window, gboolean is_minimized, GError **error);
static XfwWindowCapabilities xfw_window_x11_bspwm_get_capabilities(XfwWindow *window);

G_DEFINE_FINAL_TYPE_WITH_PRIVATE(XfwWindowX11Bspwm, xfw_window_x11_bspwm, XFW_TYPE_WINDOW_X11)

static void
xfw_window_x11_bspwm_class_init(XfwWindowX11BspwmClass *klass) {
    GObjectClass *gklass = G_OBJECT_CLASS(klass);
    XfwWindowClass *window_class = XFW_WINDOW_CLASS(klass);

    gklass->constructed = xfw_window_x11_bspwm_constructed;
    window_class->set_minimized = xfw_window_x11_bspwm_set_minimized;
    window_class->get_capabilities = xfw_window_x11_bspwm_get_capabilities;
}

static void
xfw_window_x11_bspwm_init(XfwWindowX11Bspwm *window) {
    window->priv = xfw_window_x11_bspwm_get_instance_private(window);
}

static void
xfw_window_x11_bspwm_constructed(GObject *obj) {
    XfwWindowX11Bspwm *window = XFW_WINDOW_X11_BSPWM(obj);
    XfwScreen *screen = _xfw_window_get_screen(XFW_WINDOW(window));
    XfwScreenX11 *xscreen = XFW_SCREEN_X11(screen);

    window->priv->is_bspwm = _xfw_screen_x11_is_bspwm(xscreen);

    G_OBJECT_CLASS(xfw_window_x11_bspwm_parent_class)->constructed(obj);
}

/* ---------------------------------------------------------------------------
 * Función auxiliar: ejecutar bspc con argumentos explícitos (fire-and-forget)
 * ---------------------------------------------------------------------------*/
static gboolean
xfw_window_x11_bspwm_run_bspc(GError **error, const gchar *first_arg, ...) {
    GPtrArray *argv = g_ptr_array_new();

    /* El primer elemento siempre es el binario */
    g_ptr_array_add(argv, (gpointer)"bspc");

    va_list args;
    va_start(args, first_arg);
    const gchar *arg = first_arg;
    while (arg != NULL) {
        g_ptr_array_add(argv, (gpointer)arg);
        arg = va_arg(args, const gchar *);
    }
    va_end(args);

    g_ptr_array_add(argv, NULL);  /* terminador requerido por execvp */

    GSubprocess *proc = g_subprocess_newv((const gchar *const *)argv->pdata,
                                          G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                                          G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                          error);
    g_ptr_array_free(argv, TRUE);

    if (proc == NULL) {
        return FALSE;
    }

    /* Fire-and-forget: no esperamos a que el proceso termine */
    g_object_unref(proc);
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * get_capabilities — vmethod que sobreescribe XfwWindowClass::get_capabilities
 *
 * FIX #4: bspwm nunca anuncia _NET_WM_ACTION_MINIMIZE vía EWMH (no es un
 * concepto nativo de un WM en mosaico), así que WNCK jamás incluye
 * WNCK_WINDOW_ACTION_MINIMIZE en wnck_window_get_actions(), y por lo tanto
 * la implementación base (xfw_window_x11_get_capabilities) NUNCA pone el
 * bit CAN_MINIMIZE/CAN_UNMINIMIZE, sin importar que set_minimized() esté
 * bien implementado.
 *
 * Como el panel consulta get_capabilities() ANTES de decidir si llama a
 * set_minimized() (para no ofrecer una acción "no soportada"), el panel
 * nunca llegaba a invocar nuestro código — por eso el clic solo enfocaba
 * la ventana (comportamiento por defecto), nunca la minimizaba.
 *
 * Encadenamos a la clase padre para conservar el resto de banderas
 * (CAN_MAXIMIZE, CAN_MOVE, CAN_RESIZE, etc.) y solo forzamos las dos
 * banderas relacionadas con minimizar, ya que SÍ las soportamos vía bspc.
 * ---------------------------------------------------------------------------*/
static XfwWindowCapabilities
xfw_window_x11_bspwm_get_capabilities(XfwWindow *window) {
    XfwWindowCapabilities base_caps =
        XFW_WINDOW_CLASS(xfw_window_x11_bspwm_parent_class)->get_capabilities(window);

    return base_caps
           | XFW_WINDOW_CAPABILITIES_CAN_MINIMIZE
           | XFW_WINDOW_CAPABILITIES_CAN_UNMINIMIZE;
}

/* ---------------------------------------------------------------------------
 * set_minimized — vmethod que sobreescribe XfwWindowClass::set_minimized
 * ---------------------------------------------------------------------------*/
static gboolean
xfw_window_x11_bspwm_set_minimized(XfwWindow *window, gboolean is_minimized, GError **error) {
    XfwWindowX11Private *priv = XFW_WINDOW_X11(window)->priv;
    Window xid = wnck_window_get_xid(priv->wnck_window);

    if (xid == 0) {
        g_set_error_literal(error, XFW_ERROR, XFW_ERROR_INTERNAL,
                            "Invalid window XID");
        return FALSE;
    }

    /* Representación del XID como string para el argv de bspc. */
    gchar xid_str[32];
    g_snprintf(xid_str, sizeof(xid_str), "0x%lx", (unsigned long)xid);

    if (is_minimized) {
        /* Minimizar: bspc node <xid> -g hidden=on */
        return xfw_window_x11_bspwm_run_bspc(error,
                                              "node", xid_str, "-g", "hidden=on",
                                              NULL);
    } else {
        /* Restaurar: dos comandos independientes sin depender de un shell. */
        GError *local_error = NULL;

        if (!xfw_window_x11_bspwm_run_bspc(&local_error,
                                             "node", xid_str, "-g", "hidden=off",
                                             NULL)) {
            g_propagate_error(error, local_error);
            return FALSE;
        }

        return xfw_window_x11_bspwm_run_bspc(error,
                                              "node", xid_str, "-f", ".",
                                              NULL);
    }
}

#define __XFW_WINDOW_X11_BSPWM_C__
#include "libxfce4windowing-visibility.c"