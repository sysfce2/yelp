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

    xmlNodePtr          cur;
    xmlNodePtr          cache;
    xmlXPathContextPtr  xpath;

    gboolean       link_title;
    gboolean       sort_title;

    gchar         *page_title;
    gchar         *page_desc;
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

    xmlXPathCompExprPtr  normalize;
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
    priv->normalize = xmlXPathCompile ("normalize-space(.)");
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

    if (priv->normalize)
        xmlXPathFreeCompExpr (priv->normalize);

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
        error = g_error_new (YELP_ERROR, YELP_ERROR_NOT_FOUND,
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
    gboolean editor_mode;

    GFile *gfile;
    GFileEnumerator *children;
    GFileInfo *pageinfo;

    editor_mode = yelp_settings_get_editor_mode (yelp_settings_get_default ());

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
        g_error_free (error);
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
        if (!g_str_has_suffix (filename, ".page") &&
            !(editor_mode && g_str_has_suffix (filename, ".page.stub"))) {
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
            yelp_document_set_root_id ((YelpDocument *) mallard,
                                       page_data->page_id, "index");
            yelp_document_set_page_id ((YelpDocument *) mallard,
                                       page_data->page_id, page_data->page_id);
            g_hash_table_insert (priv->pages_hash, page_data->page_id, page_data);
            yelp_document_set_page_title ((YelpDocument *) mallard,
                                          page_data->page_id,
                                          page_data->page_title);
            yelp_document_set_page_desc ((YelpDocument *) mallard,
                                         page_data->page_id,
                                         page_data->page_desc);
            yelp_document_signal ((YelpDocument *) mallard,
                                  page_data->page_id,
                                  YELP_DOCUMENT_SIGNAL_INFO,
                                  NULL);
            g_mutex_unlock (priv->mutex);
        }
        g_object_unref (pagefile);
        g_free (filename);
        g_object_unref (pageinfo);
    }

    g_mutex_lock (priv->mutex);
    priv->state = MALLARD_STATE_IDLE;
    while (priv->pending) {
        gchar *page_id = (gchar *) priv->pending->data;
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
    MallardPageData *page_data = NULL;
    gchar *real_id = NULL;
    GError *error;

    debug_print (DB_FUNCTION, "entering\n");
    debug_print (DB_ARG, "    page_id=\"%s\"\n", page_id);

    if (page_id != NULL)
        real_id = yelp_document_get_page_id ((YelpDocument *) mallard, page_id);

    if (real_id != NULL) {
        page_data = g_hash_table_lookup (priv->pages_hash, real_id);
        g_free (real_id);
    }

    if (page_data == NULL) {
        gchar *docuri = yelp_uri_get_document_uri (priv->uri);
        error = g_error_new (YELP_ERROR, YELP_ERROR_NOT_FOUND,
                             _("The page ‘%s’ was not found in the document ‘%s’."),
                             page_id, docuri);
        g_free (docuri);
        yelp_document_signal ((YelpDocument *) mallard, page_id,
                              YELP_DOCUMENT_SIGNAL_ERROR,
                              error);
        g_error_free (error);
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
        if (xmlXIncludeProcessFlags (page_data->xmldoc,
                                     XML_PARSE_DTDLOAD | XML_PARSE_NOCDATA |
                                     XML_PARSE_NOENT   | XML_PARSE_NONET   )
            < 0)
            goto done;
        page_data->cur = xmlDocGetRootElement (page_data->xmldoc);
        page_data->cache = xmlDocGetRootElement (priv->cache);
        page_data->xpath = xmlXPathNewContext (page_data->xmldoc);
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
            xmlChar *style;
            gchar **styles;
            gchar *icon = "help-contents";
            page_data->page_id = g_strdup ((gchar *) id);
            xmlSetProp (page_data->cache, BAD_CAST "id", id);
            yelp_document_set_page_id ((YelpDocument *) page_data->mallard,
                                       g_strrstr (page_data->filename, G_DIR_SEPARATOR_S),
                                       page_data->page_id);
            style = xmlGetProp (page_data->cur, BAD_CAST "style");
            if (style) {
                gint i;
                styles = g_strsplit (style, " ", -1);
                for (i = 0; styles[i] != NULL; i++) {
                    if (g_str_equal (styles[i], "task")) {
                        icon = "yelp-page-task";
                        break;
                    }
                    else if (g_str_equal (styles[i], "tip")) {
                        icon = "yelp-page-tip";
                        break;
                    }
                    else if (g_str_equal (styles[i], "ui")) {
                        icon = "yelp-page-ui";
                        break;
                    }
                    else if (g_str_equal (styles[i], "video")) {
                        icon = "yelp-page-video";
                        break;
                    }
                }
                xmlFree (style);
            }
            yelp_document_set_page_icon ((YelpDocument *) page_data->mallard,
                                         page_data->page_id, icon);
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
                if (page_data->page_title == NULL) {
                    YelpMallardDocumentPrivate *priv = GET_PRIV (page_data->mallard);
                    xmlXPathObjectPtr obj;
                    page_data->xpath->node = child;
                    obj = xmlXPathCompiledEval (priv->normalize, page_data->xpath);
                    page_data->page_title = g_strdup (obj->stringval);
                    xmlXPathFreeObject (obj);
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
    gboolean editor_mode = yelp_settings_get_editor_mode (yelp_settings_get_default ());

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
            if (xmlStrEqual (type, BAD_CAST "text")) {
                YelpMallardDocumentPrivate *priv = GET_PRIV (page_data->mallard);
                xmlXPathObjectPtr obj;
                page_data->xpath->node = child;
                obj = xmlXPathCompiledEval (priv->normalize, page_data->xpath);
                g_free (page_data->page_title);
                page_data->page_title = g_strdup (obj->stringval);
                xmlXPathFreeObject (obj);
            }
        }
        else if (xmlStrEqual (child->name, BAD_CAST "desc")) {
            YelpMallardDocumentPrivate *priv = GET_PRIV (page_data->mallard);
            xmlXPathObjectPtr obj;
            page_data->xpath->node = child;
            obj = xmlXPathCompiledEval (priv->normalize, page_data->xpath);
            page_data->page_desc = g_strdup (obj->stringval);
            xmlXPathFreeObject (obj);

            xmlAddChild (cache_node, xmlCopyNode (child, 1));
        }
        else if (xmlStrEqual (child->name, BAD_CAST "link")) {
            xmlAddChild (cache_node, xmlCopyNode (child, 1));
        }
        else if (xmlStrEqual (child->name, BAD_CAST "revision")) {
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

    mallard_page_data_cancel (page_data);
    page_data->transform = yelp_transform_new (STYLESHEET);

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

    if (g_str_has_suffix (page_data->filename, ".page.stub")) {
        gint end;
        params = yelp_settings_get_all_params (settings, 2, &end);
        params[end++] = g_strdup ("yelp.stub");
        params[end++] = g_strdup ("true()");
    }
    else
        params = yelp_settings_get_all_params (settings, 0, NULL);

    yelp_transform_start (page_data->transform,
			  page_data->xmldoc,
                          priv->cache,
			  (const gchar * const *) params);
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
    if (page_data->xpath)
        xmlXPathFreeContext (page_data->xpath);
    g_free (page_data->page_title);
    g_free (page_data->page_desc);
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