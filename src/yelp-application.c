/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * Copyright (C) 2010 Shaun McCance <shaunm@gnome.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Shaun McCance <shaunm@gnome.org>
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define G_SETTINGS_ENABLE_BACKEND

#include <dbus/dbus-glib-bindings.h>
#include <dbus/dbus-glib.h>
#include <gio/gio.h>
#include <gio/gsettingsbackend.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>

#include "yelp-settings.h"
#include "yelp-view.h"

#include "yelp-application.h"
#include "yelp-dbus.h"
#include "yelp-window.h"

#define DEFAULT_URI "ghelp:user-guide"

static gboolean editor_mode = FALSE;

static const GOptionEntry entries[] = {
    {"editor-mode", 0, 0, G_OPTION_ARG_NONE, &editor_mode, N_("Turn on editor mode"), NULL},
    { NULL }
};

typedef struct _YelpApplicationLoad YelpApplicationLoad;
struct _YelpApplicationLoad {
    YelpApplication *app;
    guint timestamp;
    gboolean new;
};

static void          yelp_application_init             (YelpApplication       *app);
static void          yelp_application_class_init       (YelpApplicationClass  *klass);
static void          yelp_application_dispose          (GObject               *object);
static void          yelp_application_finalize         (GObject               *object);

static void          application_setup                 (YelpApplication       *app);
static void          application_uri_resolved          (YelpUri               *uri,
                                                        YelpApplicationLoad   *data);
static gboolean      application_window_deleted        (YelpWindow            *window,
                                                        GdkEvent              *event,
                                                        YelpApplication       *app);
static gboolean      application_maybe_quit            (YelpApplication       *app);
static void          application_adjust_font           (GtkAction             *action,
                                                        YelpApplication       *app);
static void          application_set_font_sensitivity  (YelpApplication       *app);

G_DEFINE_TYPE (YelpApplication, yelp_application, G_TYPE_OBJECT);
#define GET_PRIV(object)(G_TYPE_INSTANCE_GET_PRIVATE ((object), YELP_TYPE_APPLICATION, YelpApplicationPrivate))

typedef struct _YelpApplicationPrivate YelpApplicationPrivate;
struct _YelpApplicationPrivate {
    DBusGConnection *connection;

    GSList *windows;
    GHashTable *windows_by_document;

    GtkActionGroup *action_group;

    GSettings *gsettings;
};

static const GtkActionEntry action_entries[] = {
    { "LargerText", GTK_STOCK_ZOOM_IN,
      N_("_Larger Text"),
      "<Control>plus",
      N_("Increase the size of the text"),
      G_CALLBACK (application_adjust_font) },
    { "SmallerText", GTK_STOCK_ZOOM_OUT,
      N_("_Smaller Text"),
      "<Control>minus",
      N_("Descrease the size of the text"),
      G_CALLBACK (application_adjust_font) }
};

static void
yelp_application_init (YelpApplication *app)
{
}

static void
yelp_application_class_init (YelpApplicationClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->dispose = yelp_application_dispose;
    object_class->finalize = yelp_application_finalize;

    dbus_g_object_type_install_info (YELP_TYPE_APPLICATION,
                                     &dbus_glib_yelp_object_info);

    g_type_class_add_private (klass, sizeof (YelpApplicationPrivate));
}

static void
yelp_application_dispose (GObject *object)
{
    YelpApplicationPrivate *priv = GET_PRIV (object);

    if (priv->connection) {
        g_object_unref (priv->connection);
        priv->connection = NULL;
    }

    if (priv->action_group) {
        g_object_unref (priv->action_group);
        priv->action_group = NULL;
    }

    if (priv->gsettings) {
        g_object_unref (priv->gsettings);
        priv->gsettings = NULL;
    }

    G_OBJECT_CLASS (yelp_application_parent_class)->dispose (object);
}

static void
yelp_application_finalize (GObject *object)
{
    YelpApplicationPrivate *priv = GET_PRIV (object);

    g_hash_table_destroy (priv->windows_by_document);

    G_OBJECT_CLASS (yelp_application_parent_class)->finalize (object);
}


static void
application_setup (YelpApplication *app)
{
    YelpApplicationPrivate *priv = GET_PRIV (app);
    YelpSettings *settings = yelp_settings_get_default ();
    GtkAction *action;

    priv->windows_by_document = g_hash_table_new_full (g_str_hash,
                                                       g_str_equal,
                                                       g_free,
                                                       NULL);
    /* FIXME: is there a better way to see if there's a real backend in use? */
    if (g_getenv ("GSETTINGS_BACKEND") == NULL) {
        gchar *keyfile = g_build_filename (g_get_user_config_dir (),
                                           "yelp", "yelp.cfg",
                                           NULL);
        g_settings_backend_setup_keyfile ("yelp-", keyfile);
        priv->gsettings = g_settings_new_with_context ("org.gnome.yelp", "yelp-");
        g_free (keyfile);
    }
    else {
        priv->gsettings = g_settings_new ("org.gnome.yelp");
    }
    g_settings_bind (priv->gsettings, "show-cursor",
                     settings, "show-text-cursor",
                     G_SETTINGS_BIND_DEFAULT);
    g_settings_bind (priv->gsettings, "font-adjustment",
                     settings, "font-adjustment",
                     G_SETTINGS_BIND_DEFAULT);

    priv->action_group = gtk_action_group_new ("ApplicationActions");
    gtk_action_group_set_translation_domain (priv->action_group, GETTEXT_PACKAGE);
    gtk_action_group_add_actions (priv->action_group,
				  action_entries, G_N_ELEMENTS (action_entries),
				  app);
    action = (GtkAction *) gtk_toggle_action_new ("ShowTextCursor",
                                                  _("Show Text _Cursor"),
                                                  NULL, NULL);
    gtk_action_group_add_action_with_accel (priv->action_group,
                                            action, "F7");
    g_settings_bind (priv->gsettings, "show-cursor",
                     action, "active",
                     G_SETTINGS_BIND_DEFAULT);
    g_object_unref (action);
    application_set_font_sensitivity (app);
}

/******************************************************************************/

GtkActionGroup *
yelp_application_get_action_group (YelpApplication  *app)
{
    YelpApplicationPrivate *priv = GET_PRIV (app);
    return priv->action_group;
}

static void
application_adjust_font (GtkAction       *action,
                         YelpApplication *app)
{
    YelpApplicationPrivate *priv = GET_PRIV (app);
    gint adjustment = g_settings_get_int (priv->gsettings, "font-adjustment");
    gint adjust = g_str_equal (gtk_action_get_name (action), "LargerText") ? 1 : -1;

    adjustment += adjust;
    g_settings_set_int (priv->gsettings, "font-adjustment", adjustment);

    application_set_font_sensitivity (app);
}

static void
application_set_font_sensitivity (YelpApplication *app)
{
    YelpApplicationPrivate *priv = GET_PRIV (app);
    YelpSettings *settings = yelp_settings_get_default ();
    GParamSpec *spec = g_object_class_find_property ((GObjectClass *) YELP_SETTINGS_GET_CLASS (settings),
                                                     "font-adjustment");
    gint adjustment = g_settings_get_int (priv->gsettings, "font-adjustment");
    if (!G_PARAM_SPEC_INT (spec)) {
        g_warning ("Expcected integer param spec for font-adjustment");
        return;
    }
    gtk_action_set_sensitive (gtk_action_group_get_action (priv->action_group, "LargerText"),
                              adjustment < ((GParamSpecInt *) spec)->maximum);
    gtk_action_set_sensitive (gtk_action_group_get_action (priv->action_group, "SmallerText"),
                              adjustment > ((GParamSpecInt *) spec)->minimum);
}

/******************************************************************************/

YelpApplication *
yelp_application_new (void)
{
    YelpApplication *app;

    app = (YelpApplication *) g_object_new (YELP_TYPE_APPLICATION, NULL);

    return app;
}

gint
yelp_application_run (YelpApplication  *app,
                      gint              argc,
                      gchar           **argv)
{
    GOptionContext *context;
    GError *error = NULL;
    DBusGProxy *proxy;
    guint request;
    YelpApplicationPrivate *priv = GET_PRIV (app);
    gchar *uri;

    g_set_application_name (N_("Help"));

    context = g_option_context_new (NULL);
    g_option_context_add_group (context, gtk_get_option_group (TRUE));
    g_option_context_add_main_entries (context, entries, GETTEXT_PACKAGE);
    g_option_context_parse (context, &argc, &argv, NULL);

    priv->connection = dbus_g_bus_get (DBUS_BUS_SESSION, &error);
    if (priv->connection == NULL) {
        g_warning ("Unable to connect to dbus: %s", error->message);
        g_error_free (error);
        return 1;
    }

    /* FIXME: canonicalize relative URI */
    if (argc > 1)
        uri = argv[1];
    else
        uri = DEFAULT_URI;

    proxy = dbus_g_proxy_new_for_name (priv->connection,
                                       DBUS_SERVICE_DBUS,
                                       DBUS_PATH_DBUS,
                                       DBUS_INTERFACE_DBUS);

    if (!org_freedesktop_DBus_request_name (proxy,
                                            "org.gnome.Yelp",
                                            0, &request,
                                            &error)) {
        g_warning ("Unable to register service: %s", error->message);
        g_error_free (error);
        g_object_unref (proxy);
        return 1;
    }

    g_object_unref (proxy);

    if (request == DBUS_REQUEST_NAME_REPLY_EXISTS ||
        request == DBUS_REQUEST_NAME_REPLY_IN_QUEUE) {
        gchar *newuri;

        if (uri && (strchr (uri, ':') || (uri[0] == '/')))
            newuri = uri;
        else {
            GFile *base, *new;
            gchar *cur = g_get_current_dir ();
            base = g_file_new_for_path (cur);
            new = g_file_resolve_relative_path (base, uri);
            newuri = g_file_get_uri (new);
            g_free (cur);
        }

        proxy = dbus_g_proxy_new_for_name (priv->connection,
                                           "org.gnome.Yelp",
                                           "/org/gnome/Yelp",
                                           "org.gnome.Yelp");
        if (!dbus_g_proxy_call (proxy, "LoadUri", &error,
                                G_TYPE_STRING, newuri,
                                G_TYPE_UINT,
                                gtk_get_current_event_time (),
                                G_TYPE_INVALID, G_TYPE_INVALID)) {
            g_warning ("Unable to notify existing process: %s\n", error->message);
            g_error_free (error);
        }

        if (newuri != uri)
            g_free (newuri);
        g_object_unref (proxy);
        return 1;
    }

    dbus_g_connection_register_g_object (priv->connection,
                                         "/org/gnome/Yelp",
                                         G_OBJECT (app));

    application_setup (app);

    yelp_settings_set_editor_mode (yelp_settings_get_default (), editor_mode);

    yelp_application_load_uri (app, uri, gtk_get_current_event_time (), NULL);

    gtk_main ();

    return 0;
}

gboolean
yelp_application_load_uri (YelpApplication  *app,
                           const gchar      *uri,
                           guint             timestamp,
                           GError          **error)
{
    YelpApplicationLoad *data;
    YelpUri *yuri;

    data = g_new (YelpApplicationLoad, 1);
    data->app = app;
    data->timestamp = timestamp;
    
    yuri = yelp_uri_new (uri);
    
    g_signal_connect (yuri, "resolved",
                      G_CALLBACK (application_uri_resolved),
                      data);
    yelp_uri_resolve (yuri);

    return TRUE;
}

void
yelp_application_new_window (YelpApplication  *app,
                             const gchar      *uri)
{
    YelpApplicationLoad *data;
    YelpUri *yuri;

    data = g_new (YelpApplicationLoad, 1);
    data->app = app;
    data->new = TRUE;

    yuri = yelp_uri_new (uri ? uri : DEFAULT_URI);

    g_signal_connect (yuri, "resolved",
                      G_CALLBACK (application_uri_resolved),
                      data);
    yelp_uri_resolve (yuri);
}

static void
application_uri_resolved (YelpUri             *uri,
                          YelpApplicationLoad *data)
{
    YelpWindow *window;
    gchar *doc_uri;
    GdkWindow *gdk_window;
    YelpApplicationPrivate *priv = GET_PRIV (data->app);

    doc_uri = yelp_uri_get_document_uri (uri);

    if (data->new)
        window = NULL;
    else
        window = g_hash_table_lookup (priv->windows_by_document, doc_uri);

    if (window == NULL) {
        window = yelp_window_new (data->app);
        priv->windows = g_slist_prepend (priv->windows, window);

        if (!data->new) {
            g_hash_table_insert (priv->windows_by_document, doc_uri, window);
            g_object_set_data (G_OBJECT (window), "doc_uri", doc_uri);
        }
        g_signal_connect (window, "delete-event",
                          G_CALLBACK (application_window_deleted), data->app);
    }
    else {
        g_free (doc_uri);
    }

    yelp_window_load_uri (window, uri);

    gtk_widget_show_all (GTK_WIDGET (window));

    /* Metacity no longer does anything useful with gtk_window_present */
    gdk_window = gtk_widget_get_window (GTK_WIDGET (window));
    if (gdk_window)
        gdk_x11_window_move_to_current_desktop (gdk_window);

    gtk_window_present_with_time (GTK_WINDOW (window), GDK_CURRENT_TIME);

    g_free (data);
}

static gboolean
application_window_deleted (YelpWindow      *window,
                              GdkEvent        *event,
                              YelpApplication *app)
{
    gchar *doc_uri; /* owned by windows_by_document */
    YelpApplicationPrivate *priv = GET_PRIV (app);

    priv->windows = g_slist_remove (priv->windows, window);
    doc_uri = g_object_get_data (G_OBJECT (window), "doc_uri");
    if (doc_uri)
        g_hash_table_remove (priv->windows_by_document, doc_uri);

    if (priv->windows == NULL)
        g_timeout_add_seconds (5, (GSourceFunc) application_maybe_quit, app);

    return FALSE;
}

static gboolean
application_maybe_quit (YelpApplication *app)
{
    YelpApplicationPrivate *priv = GET_PRIV (app);

    if (priv->windows == NULL)
        gtk_main_quit ();

    return FALSE;
}

/******************************************************************************/

static void
packages_installed (DBusGProxy     *proxy,
                    DBusGProxyCall *call,
                    gpointer        data)
{
    GError *error = NULL;
    dbus_g_proxy_end_call (proxy, call, &error, G_TYPE_INVALID);
    g_strfreev ((gchar **) data);
    if (error) {
        const gchar *err = NULL;
        if (error->domain == DBUS_GERROR) {
            if (error->code == DBUS_GERROR_SERVICE_UNKNOWN) {
                err = _("You do not have PackageKit installed.  Package installation links require PackageKit.");
            }
            else if (error->code != DBUS_GERROR_REMOTE_EXCEPTION &&
                     error->code != DBUS_GERROR_NO_REPLY) {
                err = error->message;
            }
        }
        if (err) {
            GtkWidget *dialog = gtk_message_dialog_new (NULL, 0,
                                                        GTK_MESSAGE_ERROR,
                                                        GTK_BUTTONS_CLOSE,
                                                        "%s", err);
            gtk_dialog_run ((GtkDialog *) dialog);
            gtk_widget_destroy (dialog);
        }
    }
}

void
yelp_application_install_package (YelpApplication  *app,
                                  const gchar      *pkg,
                                  const gchar      *alt)
{
    YelpApplicationPrivate *priv = GET_PRIV (app);
    guint32 xid = 0;
    DBusGProxy *proxy = dbus_g_proxy_new_for_name (priv->connection,
                                                   "org.freedesktop.PackageKit",
                                                   "/org/freedesktop/PackageKit",
                                                   "org.freedesktop.PackageKit.Modify");
    gchar **pkgs = g_new0 (gchar *, 2);
    pkgs[0] = g_strdup (pkg);

    dbus_g_proxy_begin_call (proxy, "InstallPackageNames",
                             packages_installed, pkgs, NULL,
                             G_TYPE_UINT, xid,
                             G_TYPE_STRV, pkgs,
                             G_TYPE_STRING, "",
                             G_TYPE_INVALID, G_TYPE_INVALID);
}