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
#include <glib/gstdio.h>
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

enum {
    PROP_0,
};

static void xfw_window_x11_bspwm_constructed(GObject *obj);
static gboolean xfw_window_x11_bspwm_set_minimized(XfwWindow *window, gboolean is_minimized, GError **error);

G_DEFINE_FINAL_TYPE_WITH_PRIVATE(XfwWindowX11Bspwm, xfw_window_x11_bspwm, XFW_TYPE_WINDOW_X11)

static void
xfw_window_x11_bspwm_class_init(XfwWindowX11BspwmClass *klass) {
    GObjectClass *gklass = G_OBJECT_CLASS(klass);
    XfwWindowClass *window_class = XFW_WINDOW_CLASS(klass);

    gklass->constructed = xfw_window_x11_bspwm_constructed;
    window_class->set_minimized = xfw_window_x11_bspwm_set_minimized;
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

static gboolean
xfw_window_x11_bspwm_spawn_bspc_async(const gchar *command, GError **error) {
    gchar **envp = g_get_environ();
    
    // Extract the arguments after "bspc "
    const gchar *args_start = command + 5; // Skip "bspc "
    gchar **args = g_strsplit(args_start, " ", -1);
    
    gsize n_args = g_strv_length(args);
    gchar **argv = g_new(gchar *, n_args + 2);
    argv[0] = "bspc";
    for (gsize i = 0; i < n_args; i++) {
        argv[i + 1] = args[i];
    }
    argv[n_args + 1] = NULL;

    GSubprocess *subprocess = g_subprocess_newv((const gchar *const *)argv,
                                                G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                                                G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                                error);
    g_strfreev(args);
    g_free(argv);
    g_strfreev(envp);

    if (subprocess == NULL) {
        return FALSE;
    }

    // Don't wait - fire and forget for async behavior
    g_object_unref(subprocess);
    return TRUE;
}

static gboolean
xfw_window_x11_bspwm_set_minimized(XfwWindow *window, gboolean is_minimized, GError **error) {
    XfwWindowX11Private *priv = XFW_WINDOW_X11(window)->priv;
    Window xid = wnck_window_get_xid(priv->wnck_window);

    if (xid == 0) {
        if (error != NULL) {
            *error = g_error_new_literal(XFW_ERROR, XFW_ERROR_INTERNAL, "Invalid window XID");
        }
        return FALSE;
    }

    gchar *command;
    if (is_minimized) {/*
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

G_DEFINE_FINAL_TYPE_WITH_PRIVATE(XfwWindowX11Bspwm, xfw_window_x11_bspwm, XFW_TYPE_WINDOW_X11)

static void
xfw_window_x11_bspwm_class_init(XfwWindowX11BspwmClass *klass) {
    GObjectClass *gklass = G_OBJECT_CLASS(klass);
    XfwWindowClass *window_class = XFW_WINDOW_CLASS(klass);

    gklass->constructed = xfw_window_x11_bspwm_constructed;
    window_class->set_minimized = xfw_window_x11_bspwm_set_minimized;
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
 *
 * FIX #2 + #3: Reemplaza la función xfw_window_x11_bspwm_spawn_bspc_async()
 * original, que tenía dos problemas críticos:
 *
 *   a) Pasaba "&&" como argumento literal a g_subprocess_newv(), que ejecuta
 *      el binario directamente sin ningún shell.  El operador "&&" es una
 *      construcción de shell y bspc lo rechazaba como argumento desconocido.
 *
 *   b) La función recibía un string ya formateado ("bspc node ..."), lo
 *      partía saltando los primeros 5 caracteres ("bspc ") con aritmética de
 *      punteros y luego hacía strsplit(), diseño frágil que se rompe si el
 *      formato del string cambia.
 *
 * Esta función recibe el nombre del subcomando y sus argumentos directamente,
 * sin construir ningún string intermedio, y los pasa tal cual a execvp().
 *
 * Uso:
 *   xfw_window_x11_bspwm_run_bspc(&err, "node", "0x1234", "-g", "hidden=on", NULL);
 *
 * La lista de argumentos DEBE terminar en NULL.
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
 * set_minimized — vmethod que sobreescribe XfwWindowClass::set_minimized
 *
 * FIX #3: La sintaxis "bspc node -i <xid>" es inválida: bspc node NO tiene
 * un flag -i para seleccionar un nodo por XID.  La sintaxis correcta es
 * pasar el XID directamente como selector posicional:
 *
 *   bspc node <xid> -g hidden=on
 *   bspc node <xid> -g hidden=off
 *   bspc node <xid> -f .        ← focalizar el nodo tras restaurarlo
 *
 * FIX #2: Para desminimizar se necesitan dos comandos secuenciales.  En lugar
 * de concatenarlos con "&&" en un solo string (que no funciona sin shell),
 * ejecutamos dos GSubprocess separados.
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

    /* Representación del XID como string para el argv de bspc.
     * bspc acepta tanto formato decimal como "0x"-prefixed hex; usamos hex
     * porque es la notación canónica de XIDs en toda la documentación de bspwm. */
    gchar xid_str[32];
    g_snprintf(xid_str, sizeof(xid_str), "0x%lx", (unsigned long)xid);

    if (is_minimized) {
        /* Minimizar: bspc node <xid> -g hidden=on */
        return xfw_window_x11_bspwm_run_bspc(error,
                                              "node", xid_str, "-g", "hidden=on",
                                              NULL);
    } else {
        /* Restaurar: dos comandos independientes.
         *
         * Paso 1: quitar el flag hidden.
         * Paso 2: mover el foco al nodo.
         *
         * Usamos "-f ." (punto = nodo actual en el contexto de bspwm) porque
         * tras hidden=off bspwm ya seleccionó internamente ese nodo.  Si se
         * prefiere foco explícito por XID se puede usar xid_str en lugar de ".".
         *
         * Si el primer comando falla, propagamos el error y no ejecutamos el
         * segundo, evitando focalizar un nodo que no se restauró correctamente. */
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

        // Minimize: bspc node -i <xid> -g hidden=on
        command = g_strdup_printf("bspc node -i 0x%lx -g hidden=on", (unsigned long)xid);
    } else {
        // Unminimize: bspc node -i <xid> -g hidden=off && bspc node -f <xid>
        command = g_strdup_printf("bspc node -i 0x%lx -g hidden=off && bspc node -f 0x%lx",
                                  (unsigned long)xid, (unsigned long)xid);
    }

    gboolean result = xfw_window_x11_bspwm_spawn_bspc_async(command, error);
    g_free(command);

    return result;
}

#define __XFW_WINDOW_X11_BSPWM_C__
#include "libxfce4windowing-visibility.c"