/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * Copyright (C) 2009 Shaun McCance  <shaunm@gnome.org>
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
 * Author: Shaun McCance  <shaunm@gnome.org>
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <glib.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <libxml/parser.h>
#include <libxml/parserInternals.h>
#include <libxml/xinclude.h>

#include "yelp-error.h"
#include "yelp-mallard-document.h"
#include "yelp-settings.h"
#include "yelp-transform.h"
#include "yelp-debug.h"

#define STYLESHEET DATADIR"/yelp/xslt/mal2html.xsl"
#define MALLARD_NS "http://projectmallard.org/1.0/"

typedef enum {
    MALLARD_STATE_BLANK,
    MALLARD_STATE_THINKING,
    MALLARD_STATE_IDLE,
    MALLARD_STATE_STOP
} MallardState;

typedef struct {
    YelpMallardDocument *mallard;

    gchar         *page_id;
    gchar         *filename;
    xmlDocPtr      xmldoc;
    YelpTransform *transform;

    guint          chunk_ready;
    guint          finished;
    guint          error;

    xmlNodePtr     cur;
    xmlNodePtr     cache;
    gboolean       link_title;
    gboolean       sort_title;
} MallardPageData;

static void           yelp_mallard_document_class_init (YelpMallardDocumentClass *klass);
static void           yelp_mallard_document_init       (YelpMallardDocument      *mallard);
static void           yelp_mallard_document_dispose    (GObject                  *object);
static void           yelp_mallard_document_finalize   (GObject                  *object);

static gboolean       mallard_request_page      (YelpDocument         *document,
                                                 const gchar          *page_id,
                                                 GCancellable         *cancellable,
                                                 YelpDocumentCallback  callback,
                                                 gpointer              user_data);

static void           transform_chunk_ready     (YelpTransform        *transform,
                                                 gchar                *chunk_id,
                                                 MallardPageData      *page_data);
static void           transform_finished        (YelpTransform        *transform,
                                                 MallardPageData      *page_data);
static void           transform_error           (YelpTransform        *transform,
                                                 MallardPageData      *page_data);

static void           mallard_think             (YelpMallardDocument  *mallard);
static void           mallard_try_run           (YelpMallardDocument  *mallard,
                                                 const gchar          *page_id);

static void           mallard_page_data_cancel  (MallardPageData      *page_data);
static void           mallard_page_data_walk    (MallardPageData      *page_data);
static void           mallard_page_data_info    (MallardPageData      *page_data,
                                                 xmlNodePtr            info_node,
                                                 xmlNodePtr            cache_node);
static void           mallard_page_data_run     (MallardPageData      *page_data);
static void           mallard_page_data_free    (MallardPageData      *page_data);


G_DEFINE_TYPE (YelpMallardDocument, yelp_mallard_document, YELP_TYPE_DOCUMENT);
#define GET_PRIV(object) (G_TYPE_INSTANCE_GET_PRIVATE ((object), YELP_TYPE_MALLARD_DOCUMENT, YelpMallardDocumentPrivate))

typedef struct _YelpMallardDocumentPrivate  YelpMallardDocumentPrivate;
struct _YelpMallardDocumentPrivate {
    YelpUri       *uri;
    MallardState   state;

    GMutex        *mutex;
    GThread       *thread;
    gboolean       thread_running;
    GSList        *pending;

    xmlDocPtr      cache;
    xmlNsPtr       cache_ns;
    GHashTable    *pages_hash;
};

/******************************************************************************/

static void
yelp_mallard_document_class_init (YelpMallardDocumentClass *klass)
{
    GObjectClass      *object_class   = G_OBJECT_CLASS (klass);
    YelpDocumentClass *document_class = YELP_DOCUMENT_CLASS (klass);

    object_class->dispose = yelp_mallard_document_dispose;
    object_class->finalize = yelp_mallard_document_finalize;

    document_class->request_page = mallard_request_page;

    g_type_class_add_private (klass, sizeof (YelpMallardDocumentPrivate));
}

static void
yelp_mallard_document_init (YelpMallardDocument *mallard)
{
    YelpMallardDocumentPrivate *priv = GET_PRIV (mallard);
    xmlNodePtr cur;

    priv->mutex = g_mutex_new ();

    priv->thread_running = FALSE;

    priv->cache = xmlNewDoc (BAD_CAST "1.0");
    priv->cache_ns = xmlNewNs (NULL, BAD_CAST MALLARD_NS, BAD_CAST "mal");
    cur = xmlNewDocNode (priv->cache, priv->cache_ns, BAD_CAST "cache", NULL);
    xmlDocSetRootElement (priv->cache, cur);
    priv->cache_ns->next = cur->nsDef;
    cur->nsDef = priv->cache_ns;
    priv->pages_hash = g_hash_table_new_full (g_str_hash, g_str_equal,
                                              NULL,
                                              (GDestroyNotify) mallard_page_data_free);
}

static void
yelp_mallard_document_dispose (GObject *object)
{
    G_OBJECT_CLASS (yelp_mallard_document_parent_class)->dispose (object);
}

static void
yelp_mallard_document_finalize (GObject *object)
{
    YelpMallardDocumentPrivate *priv = GET_PRIV (object);

    g_object_unref (priv->uri);
    g_mutex_free (priv->mutex);
    g_hash_table_destroy (priv->pages_hash);

    G_OBJECT_CLASS (yelp_mallard_document_parent_class)->finalize (object);
}

/******************************************************************************/

YelpDocument *
yelp_mallard_document_new (YelpUri *uri)
{
    YelpMallardDocument *mallard;
    YelpMallardDocumentPrivate *priv;

    g_return_val_if_fail (uri != NULL, NULL);

    mallard = (YelpMallardDocument *) g_object_new (YELP_TYPE_MALLARD_DOCUMENT, NULL);
    priv = GET_PRIV (mallard);
    priv->uri = g_object_ref (uri);

    yelp_document_set_page_id ((YelpDocument *) mallard, NULL, "index");
    yelp_document_set_page_id ((YelpDocument *) mallard, "index", "index");

    return (YelpDocument *) mallard;
}


static gboolean
mallard_request_page (YelpDocument         *document,
                      const gchar          *page_id,
                      GCancellable         *cancellable,
                      YelpDocumentCallback  callback,
                      gpointer              user_data)
{
    YelpMallardDocumentPrivate *priv = GET_PRIV (document);
    gchar *docuri;
    GError *error;
    gboolean handled;

    debug_print (DB_FUNCTION, "entering\n");
    debug_print (DB_ARG, "    page_id=\"%s\"\n", page_id);

    if (page_id == NULL)
        page_id = "index";

    handled =
        YELP_DOCUMENT_CLASS (yelp_mallard_document_parent_class)->request_page (document,
                                                                                page_id,
                                                                                cancellable,
                                                                                callback,
                                                                                user_data);
    if (handled) {
        return TRUE;
    }

    g_mutex_lock (priv->mutex);

    if (priv->state == MALLARD_STATE_BLANK) {
        priv->state = MALLARD_STATE_THINKING;
        priv->thread_running = TRUE;
        g_object_ref (document);
        priv->thread = g_thread_create ((GThreadFunc) mallard_think,
                                        document, FALSE, NULL);
    }

    switch (priv->state) {
    case MALLARD_STATE_THINKING:
        priv->pending = g_slist_prepend (priv->pending, (gpointer) g_strdup (page_id));
        break;
    case MALLARD_STATE_IDLE:
        mallard_try_run ((YelpMallardDocument *) document, page_id);
        break;
    case MALLARD_STATE_BLANK:
    case MALLARD_STATE_STOP:
        docuri = yelp_uri_get_document_uri (priv->uri);
        error = g_error_new (YELP_ERROR, YELP_ERROR_PROCESSING,
                             _("The page ‘%s’ was not found in the document ‘%s’."),
                             page_id, docuri);
        g_free (docuri);
        yelp_document_signal (document, page_id,
                              YELP_DOCUMENT_SIGNAL_ERROR,
                              error);
        g_error_free (error);
	break;
    }

    g_mutex_unlock (priv->mutex);

    return FALSE;
}

/******************************************************************************/

static void
mallard_think (YelpMallardDocument *mallard)
{
    YelpMallardDocumentPrivate *priv = GET_PRIV (mallard);
    GError *error = NULL;
    YelpDocument *document;
    gchar **search_path;

    GFile *gfile;
    GFileEnumerator *children;
    GFileInfo *pageinfo;

    search_path = yelp_uri_get_search_path (priv->uri);

    if (!search_path || search_path[0] == NULL ||
        !g_file_test (search_path[0], G_FILE_TEST_IS_DIR)) {
        /* This basically only happens when someone passes an actual directory
           manually, which will have a singleton search path.
         */
        error = g_error_new (YELP_ERROR, YELP_ERROR_NOT_FOUND,
                             _("The directory ‘%s’ does not exist."),
                             search_path[0]);
	yelp_document_error_pending ((YelpDocument *) document, error);
	goto done;
    }

    gfile = yelp_uri_get_file (priv->uri);
    children = g_file_enumerate_children (gfile,
                                          G_FILE_ATTRIBUTE_STANDARD_NAME,
                                          G_FILE_QUERY_INFO_NONE,
                                          NULL, NULL);
    while ((pageinfo = g_file_enumerator_next_file (children, NULL, NULL))) {
        MallardPageData *page_data;
        gchar *filename;
        GFile *pagefile;
        filename = g_file_info_get_attribute_as_string (pageinfo,
                                                        G_FILE_ATTRIBUTE_STANDARD_NAME);
        if (!g_str_has_suffix (filename, ".page")) {
            g_free (filename);
            g_object_unref (pageinfo);
            continue;
        }
        page_data = g_new0 (MallardPageData, 1);
        page_data->mallard = mallard;
        pagefile = g_file_resolve_relative_path (gfile, filename);
        page_data->filename = g_file_get_path (pagefile);
        mallard_page_data_walk (page_data);
        if (page_data->page_id == NULL) {
            mallard_page_data_free (page_data);
        } else {
            g_mutex_lock (priv->mutex);
            yelp_document_set_page_id ((YelpDocument *) mallard,
                                       page_data->page_id, page_data->page_id);
            g_hash_table_insert (priv->pages_hash, page_data->page_id, page_data);
            g_mutex_unlock (priv->mutex);
        }
        g_object_unref (pagefile);
        g_free (filename);
        g_object_unref (pageinfo);
    }

    g_mutex_lock (priv->mutex);
    priv->state = MALLARD_STATE_IDLE;
    while (priv->pending) {
        gchar *page_id;
        page_id = (gchar *) priv->pending->data;
        mallard_try_run (mallard, page_id);
        g_free (page_id);
        priv->pending = g_slist_delete_link (priv->pending, priv->pending);
    }
    g_mutex_unlock (priv->mutex);

 done:
    g_object_unref (children);
    g_object_unref (gfile);

    priv->thread_running = FALSE;
    g_object_unref (mallard);
}

static void
mallard_try_run (YelpMallardDocument *mallard,
                 const gchar         *page_id)
{
    /* We expect to be in a locked mutex when this function is called. */
    YelpMallardDocumentPrivate *priv = GET_PRIV (mallard);
    MallardPageData *page_data;
    GError *error;

    page_data = g_hash_table_lookup (priv->pages_hash, page_id);
    if (page_data == NULL) {
        gchar *docuri = yelp_uri_get_document_uri (priv->uri);
        error = g_error_new (YELP_ERROR, YELP_ERROR_NOT_FOUND,
                             _("The page ‘%s’ was not found in the document ‘%s’."),
                             page_id, docuri);
        g_free (docuri);
        yelp_document_signal ((YelpDocument *) mallard, page_id,
                              YELP_DOCUMENT_SIGNAL_ERROR,
                              error);
        return;
    }

    mallard_page_data_run (page_data);
}

/******************************************************************************/
/** MallardPageData ***********************************************************/

static void
mallard_page_data_cancel (MallardPageData *page_data)
{
    if (page_data->transform) {
        if (page_data->chunk_ready) {
            g_signal_handler_disconnect (page_data->transform, page_data->chunk_ready);
            page_data->chunk_ready = 0;
        }
        if (page_data->finished) {
            g_signal_handler_disconnect (page_data->transform, page_data->finished);
            page_data->finished = 0;
        }
        if (page_data->error) {
            g_signal_handler_disconnect (page_data->transform, page_data->error);
            page_data->error = 0;
        }
        yelp_transform_cancel (page_data->transform);
        g_object_unref (page_data->transform);
        page_data->transform = NULL;
    }
}

static void
mallard_page_data_walk (MallardPageData *page_data)
{
    YelpMallardDocumentPrivate *priv = GET_PRIV (page_data->mallard);
    xmlParserCtxtPtr parserCtxt = NULL;
    xmlChar *id = NULL;

    if (page_data->cur == NULL) {
        parserCtxt = xmlNewParserCtxt ();
        page_data->xmldoc = xmlCtxtReadFile (parserCtxt,
                                             (const char *) page_data->filename, NULL,
                                             XML_PARSE_DTDLOAD | XML_PARSE_NOCDATA |
                                             XML_PARSE_NOENT   | XML_PARSE_NONET   );
        if (page_data->xmldoc == NULL)
            goto done;
        page_data->cur = xmlDocGetRootElement (page_data->xmldoc);
        page_data->cache = xmlDocGetRootElement (priv->cache);
        mallard_page_data_walk (page_data);
    } else {
        xmlNodePtr child, oldcur, oldcache, info;

        id = xmlGetProp (page_data->cur, BAD_CAST "id");
        if (id == NULL)
            goto done;

        page_data->cache = xmlNewChild (page_data->cache,
                                        priv->cache_ns,
                                        page_data->cur->name,
                                        NULL);

        if (xmlStrEqual (page_data->cur->name, BAD_CAST "page")) {
            page_data->page_id = g_strdup ((gchar *) id);
            xmlSetProp (page_data->cache, BAD_CAST "id", id);
        } else {
            gchar *newid = g_strdup_printf ("%s#%s", page_data->page_id, id);
            xmlSetProp (page_data->cache, BAD_CAST "id", BAD_CAST newid);
            g_free (newid);
        }

        info = xmlNewChild (page_data->cache,
                            priv->cache_ns,
                            BAD_CAST "info", NULL);
        page_data->link_title = FALSE;
        page_data->sort_title = FALSE;
        for (child = page_data->cur->children; child; child = child->next) {
            if (child->type != XML_ELEMENT_NODE)
                continue;
            if (xmlStrEqual (child->name, BAD_CAST "info")) {
                mallard_page_data_info (page_data, child, info);
            }
            else if (xmlStrEqual (child->name, BAD_CAST "title")) {
                xmlNodePtr node;
                xmlNodePtr title_node = xmlNewChild (page_data->cache,
                                                     priv->cache_ns,
                                                     BAD_CAST "title", NULL);
                for (node = child->children; node; node = node->next) {
                    xmlAddChild (title_node, xmlCopyNode (node, 1));
                }
                if (!page_data->link_title) {
                    xmlNodePtr title_node = xmlNewChild (info,
                                                         priv->cache_ns,
                                                         BAD_CAST "title", NULL);
                    xmlSetProp (title_node, BAD_CAST "type", BAD_CAST "link");
                    for (node = child->children; node; node = node->next) {
                        xmlAddChild (title_node, xmlCopyNode (node, 1));
                    }
                }
                if (!page_data->sort_title) {
                    xmlNodePtr title_node = xmlNewChild (info,
                                                         priv->cache_ns,
                                                         BAD_CAST "title", NULL);
                    xmlSetProp (title_node, BAD_CAST "type", BAD_CAST "sort");
                    for (node = child->children; node; node = node->next) {
                        xmlAddChild (title_node, xmlCopyNode (node, 1));
                    }
                }
            }
            else if (xmlStrEqual (child->name, BAD_CAST "section")) {
                oldcur = page_data->cur;
                oldcache = page_data->cache;
                page_data->cur = child;
                mallard_page_data_walk (page_data);
                page_data->cur = oldcur;
                page_data->cache = oldcache;
            }
        }
    }

 done:
    if (id)
        xmlFree (id);
    if (parserCtxt)
	xmlFreeParserCtxt (parserCtxt);
}

static void
mallard_page_data_info (MallardPageData *page_data,
                        xmlNodePtr       info_node,
                        xmlNodePtr       cache_node)
{
    xmlNodePtr child;

    for (child = info_node->children; child; child = child->next) {
        if (xmlStrEqual (child->name, BAD_CAST "info")) {
            mallard_page_data_info (page_data, child, cache_node);
        }
        else if (xmlStrEqual (child->name, BAD_CAST "title")) {
            xmlNodePtr node, title_node;
            xmlChar *type, *role;
            title_node = xmlCopyNode (child, 1);
            xmlAddChild (cache_node, title_node);

            type = xmlGetProp (child, BAD_CAST "type");
            role = xmlGetProp (child, BAD_CAST "role");

            if (xmlStrEqual (type, BAD_CAST "link") && role == NULL)
                page_data->link_title = TRUE;
            if (xmlStrEqual (type, BAD_CAST "sort"))
                page_data->sort_title = TRUE;
        }
        else if (xmlStrEqual (child->name, BAD_CAST "desc") ||
                 xmlStrEqual (child->name, BAD_CAST "link")) {
            xmlAddChild (cache_node, xmlCopyNode (child, 1));
        }
    }
}

static void
mallard_page_data_run (MallardPageData *page_data)
{
    YelpSettings *settings = yelp_settings_get_default ();
    YelpMallardDocumentPrivate *priv = GET_PRIV (page_data->mallard);
    gint i, ix;
    gchar **params = NULL;
    page_data->transform = yelp_transform_new ();
    /* FIXME: handle error */
    yelp_transform_set_stylesheet (page_data->transform, STYLESHEET, NULL);

    page_data->chunk_ready =
        g_signal_connect (page_data->transform, "chunk-ready",
                          (GCallback) transform_chunk_ready,
                          page_data);
    page_data->finished =
        g_signal_connect (page_data->transform, "finished",
                          (GCallback) transform_finished,
                          page_data);
    page_data->error =
        g_signal_connect (page_data->transform, "error",
                          (GCallback) transform_error,
                          page_data);

    params = g_new0 (gchar *,
                     (2*YELP_SETTINGS_NUM_COLORS) + (2*YELP_SETTINGS_NUM_ICONS) + 3);
    for (i = 0; i < YELP_SETTINGS_NUM_COLORS; i++) {
        gchar *val;
        ix = 2 * i;
        params[ix] = g_strdup (yelp_settings_get_color_param (i));
        val = yelp_settings_get_color (settings, i);
        params[ix + 1] = g_strdup_printf ("\"%s\"", val);
        g_free (val);
    }
    for (i = 0; i < YELP_SETTINGS_NUM_ICONS; i++) {
        gchar *val;
        ix = 2 * (YELP_SETTINGS_NUM_COLORS + i);
        params[ix] = g_strdup (yelp_settings_get_icon_param (i));
        val = yelp_settings_get_icon (settings, i);
        params[ix + 1] = g_strdup_printf ("\"%s\"", val);
        g_free (val);
    }
    ix = 2 * (YELP_SETTINGS_NUM_COLORS + YELP_SETTINGS_NUM_ICONS);
    params[ix++] = g_strdup ("theme.icon.admon.size");
    params[ix++] = g_strdup_printf ("%i", yelp_settings_get_icon_size (settings));
    params[ix] = NULL;

    yelp_transform_set_aux (page_data->transform,
                            priv->cache);

    /* FIXME: handle error */
    yelp_transform_start (page_data->transform,
			  page_data->xmldoc,
			  (const gchar * const *) params,
                          NULL);
    g_strfreev (params);
}

static void
mallard_page_data_free (MallardPageData *page_data)
{
    mallard_page_data_cancel (page_data);
    g_free (page_data->page_id);
    g_free (page_data->filename);
    if (page_data->xmldoc)
        xmlFreeDoc (page_data->xmldoc);
    g_free (page_data);
}

/******************************************************************************/
/** YelpTransform *************************************************************/

static void
transform_chunk_ready (YelpTransform   *transform,
                       gchar           *chunk_id,
                       MallardPageData *page_data)
{
    YelpMallardDocumentPrivate *priv;
    gchar *content;

    debug_print (DB_FUNCTION, "entering\n");

    g_assert (page_data != NULL && page_data->mallard != NULL &&
              YELP_IS_MALLARD_DOCUMENT (page_data->mallard));
    g_assert (transform == page_data->transform);

    priv = GET_PRIV (page_data->mallard);

    if (priv->state == MALLARD_STATE_STOP) {
        mallard_page_data_cancel (page_data);
        return;
    }

    content = yelp_transform_take_chunk (transform, chunk_id);
    yelp_document_give_contents (YELP_DOCUMENT (page_data->mallard),
                                 chunk_id,
                                 content,
                                 "application/xhtml+xml");

    yelp_document_signal (YELP_DOCUMENT (page_data->mallard),
                          chunk_id,
                          YELP_DOCUMENT_SIGNAL_CONTENTS,
                          NULL);
}

static void
transform_finished (YelpTransform   *transform,
                    MallardPageData *page_data)
{
    YelpMallardDocumentPrivate *priv;

    g_assert (page_data != NULL && page_data->mallard != NULL &&
              YELP_IS_MALLARD_DOCUMENT (page_data->mallard));
    g_assert (transform == page_data->transform);

    priv = GET_PRIV (page_data->mallard);

    if (priv->state == MALLARD_STATE_STOP) {
        mallard_page_data_cancel (page_data);
        return;
    }

    mallard_page_data_cancel (page_data);

    if (page_data->xmldoc)
	xmlFreeDoc (page_data->xmldoc);
    page_data->xmldoc = NULL;
}

static void
transform_error (YelpTransform   *transform,
                 MallardPageData *page_data)
{
    YelpMallardDocumentPrivate *priv;
    GError *error;

    g_assert (page_data != NULL && page_data->mallard != NULL &&
              YELP_IS_MALLARD_DOCUMENT (page_data->mallard));
    g_assert (transform == page_data->transform);

    priv = GET_PRIV (page_data->mallard);

    if (priv->state == MALLARD_STATE_STOP) {
        mallard_page_data_cancel (page_data);
        return;
    }

    error = yelp_transform_get_error (transform);
    yelp_document_error_pending ((YelpDocument *) (page_data->mallard), error);
    g_error_free (error);

    mallard_page_data_cancel (page_data);
}