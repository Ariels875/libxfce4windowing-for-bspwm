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

/* ===========================================================================
 * BLOQUE DE DEPURACIÓN TEMPORAL
 * ---------------------------------------------------------------------------
 * Todas las líneas marcadas con "[bspwm-debug]" son temporales, solo para
 * encontrar en qué punto exacto se rompe la cadena de llamadas:
 *
 *   panel hace clic -> xfw_window_get_capabilities() -> ¿CAN_MINIMIZE=1?
 *     -> xfw_window_set_minimized() -> xfw_window_x11_bspwm_set_minimized()
 *       -> xfw_window_x11_bspwm_run_bspc() -> g_subprocess_newv() -> bspc
 *
 * Usa g_printerr() (va directo a stderr, sin buffering) en vez de g_debug()
 * para que aparezca SIEMPRE, sin depender de variables de entorno como
 * G_MESSAGES_DEBUG.
 *
 * CÓMO VER ESTA SALIDA:
 * xfce4-panel normalmente no tiene una terminal visible adjunta cuando lo
 * arranca el autostart de la sesión, así que stderr no se ve en ningún lado
 * a simple vista. Para depurar:
 *
 *   1. Mata el panel actual:      pkill -x xfce4-panel
 *   2. Lánzalo a mano, en una terminal, SIN segundo plano:
 *        xfce4-panel
 *      (déjalo corriendo en esa terminal, no le pongas &)
 *   3. Haz clic en minimizar/restaurar desde el panel
 *   4. Mira lo que imprime esa misma terminal
 *
 * Cuando termines de depurar, busca el comentario "FIN DEL BLOQUE DE
 * DEPURACIÓN" más abajo y quita todas las líneas g_printerr().
 * ===========================================================================*/

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

    /* [bspwm-debug] Confirma que ESTA subclase realmente se está
     * instanciando, y qué devolvió la detección de bspwm. Si esta línea
     * NUNCA aparece en la terminal, el problema es anterior a todo lo que
     * hemos tocado: la fábrica de tipos en xfw-screen-x11.c no está
     * eligiendo XfwWindowX11Bspwm en absoluto, y nada de lo que hagamos
     * en este archivo puede importar. */
    g_printerr("[bspwm-debug] constructed() llamado. is_bspwm=%d\n",
               window->priv->is_bspwm);

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

    /* [bspwm-debug] Imprime el comando EXACTO que se va a ejecutar, tal
     * como queda construido en argv, token por token (así detectamos de
     * inmediato cualquier problema de formato/espacios/comillas). */
    {
        GString *cmd_repr = g_string_new("[bspwm-debug] argv ->");
        for (guint i = 0; i < argv->len - 1; i++) {
            g_string_append_printf(cmd_repr, " [%s]", (const gchar *)argv->pdata[i]);
        }
        g_printerr("%s\n", cmd_repr->str);
        g_string_free(cmd_repr, TRUE);
    }

    /* [bspwm-debug] YA NO silenciamos stdout/stderr del subproceso.
     * G_SUBPROCESS_FLAGS_NONE hereda los descriptores del proceso padre
     * (xfce4-panel), así que cualquier cosa que bspc imprima en stdout o
     * stderr aparecerá en la terminal donde lanzaste xfce4-panel a mano. */
    GSubprocess *proc = g_subprocess_newv((const gchar *const *)argv->pdata,
                                          G_SUBPROCESS_FLAGS_NONE,
                                          error);
    g_ptr_array_free(argv, TRUE);

    if (proc == NULL) {
        g_printerr("[bspwm-debug] g_subprocess_newv() FALLÓ AL LANZAR: %s\n",
                   (error != NULL && *error != NULL) ? (*error)->message : "(sin mensaje de error)");
        return FALSE;
    }

    g_printerr("[bspwm-debug] subproceso bspc lanzado correctamente (fire-and-forget)\n");

    /* Fire-and-forget: no esperamos a que el proceso termine */
    g_object_unref(proc);
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * get_capabilities — vmethod que sobreescribe XfwWindowClass::get_capabilities
 * ---------------------------------------------------------------------------*/
static XfwWindowCapabilities
xfw_window_x11_bspwm_get_capabilities(XfwWindow *window) {
    XfwWindowCapabilities base_caps =
        XFW_WINDOW_CLASS(xfw_window_x11_bspwm_parent_class)->get_capabilities(window);

    XfwWindowCapabilities final_caps = base_caps
                                        | XFW_WINDOW_CAPABILITIES_CAN_MINIMIZE
                                        | XFW_WINDOW_CAPABILITIES_CAN_UNMINIMIZE;

    /* [bspwm-debug] Si esta línea NUNCA aparece al hacer clic, el panel
     * no está consultando capacidades en absoluto antes de actuar (otra
     * posible explicación distinta a la que descartamos), o simplemente
     * esta vmethod tampoco se está enlazando correctamente. */
    g_printerr("[bspwm-debug] get_capabilities() llamado. base=0x%x final=0x%x (CAN_MINIMIZE=%d, CAN_UNMINIMIZE=%d)\n",
               base_caps, final_caps,
               (final_caps & XFW_WINDOW_CAPABILITIES_CAN_MINIMIZE) != 0,
               (final_caps & XFW_WINDOW_CAPABILITIES_CAN_UNMINIMIZE) != 0);

    return final_caps;
}

/* ---------------------------------------------------------------------------
 * set_minimized — vmethod que sobreescribe XfwWindowClass::set_minimized
 * ---------------------------------------------------------------------------*/
static gboolean
xfw_window_x11_bspwm_set_minimized(XfwWindow *window, gboolean is_minimized, GError **error) {
    /* [bspwm-debug] Si esta línea NUNCA aparece al hacer clic en
     * minimizar/restaurar, confirmamos que el panel JAMÁS llega a llamar
     * esta función — el bloqueo está en otro lado (capacidades, tipo de
     * objeto instanciado, o el propio plugin de panel). Si SÍ aparece,
     * el problema está más abajo, en run_bspc() o en bspc mismo. */
    g_printerr("[bspwm-debug] >>> set_minimized() llamado, is_minimized=%d\n", is_minimized);

    XfwWindowX11Private *priv = XFW_WINDOW_X11(window)->priv;
    Window xid = wnck_window_get_xid(priv->wnck_window);

    if (xid == 0) {
        g_printerr("[bspwm-debug] XID inválido (0) — wnck_window_get_xid falló\n");
        g_set_error_literal(error, XFW_ERROR, XFW_ERROR_INTERNAL,
                            "Invalid window XID");
        return FALSE;
    }

    /* Representación del XID como string para el argv de bspc. */
    gchar xid_str[32];
    g_snprintf(xid_str, sizeof(xid_str), "0x%lx", (unsigned long)xid);
    g_printerr("[bspwm-debug] XID resuelto: %s\n", xid_str);

    if (is_minimized) {
        /* Minimizar: bspc node <xid> -g hidden=on */
        gboolean ok = xfw_window_x11_bspwm_run_bspc(error,
                                                      "node", xid_str, "-g", "hidden=on",
                                                      NULL);
        g_printerr("[bspwm-debug] resultado de minimizar: %s\n", ok ? "OK" : "FALLÓ");
        return ok;
    } else {
        /* Restaurar.
         *
         * CAMBIO respecto a la versión anterior: añadimos el calificador
         * ".hidden" al selector para el primer comando. Por defecto, los
         * selectores de bspc EXCLUYEN nodos ocultos al buscar coincidencias
         * — si el nodo ya está hidden=on, un selector "pelado" (solo el
         * XID) probablemente no lo encuentra. ".hidden" le dice a bspc
         * "sí, busca también entre los nodos ocultos".
         *
         * También reemplazamos el selector "." (no confirmado como válido
         * en la gramática de bspc) por el XID explícito que ya tenemos. */
        gchar hidden_selector[40];
        g_snprintf(hidden_selector, sizeof(hidden_selector), "%s.hidden", xid_str);
        g_printerr("[bspwm-debug] selector con calificador .hidden: %s\n", hidden_selector);

        GError *local_error = NULL;
        gboolean ok1 = xfw_window_x11_bspwm_run_bspc(&local_error,
                                                       "node", hidden_selector, "-g", "hidden=off",
                                                       NULL);
        g_printerr("[bspwm-debug] resultado de hidden=off: %s\n", ok1 ? "OK" : "FALLÓ");

        if (!ok1) {
            g_propagate_error(error, local_error);
            return FALSE;
        }

        gboolean ok2 = xfw_window_x11_bspwm_run_bspc(error,
                                                       "node", xid_str, "-f", xid_str,
                                                       NULL);
        g_printerr("[bspwm-debug] resultado de focus: %s\n", ok2 ? "OK" : "FALLÓ");
        return ok2;
    }
}
/* ======================= FIN DEL BLOQUE DE DEPURACIÓN ===================== */

#define __XFW_WINDOW_X11_BSPWM_C__
#include "libxfce4windowing-visibility.c"