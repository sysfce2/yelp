/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/*
 * Copyright (C) 2003 Shaun McCance  <shaunm@gnome.org>
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
#include <libxml/parser.h>
#include <libxml/parserInternals.h>
#include <libxml/xinclude.h>
#include <libxslt/xslt.h>
#include <libxslt/templates.h>
#include <libxslt/transform.h>
#include <libxslt/extensions.h>
#include <libxslt/xsltInternals.h>
#include <libxslt/xsltutils.h>

#include "yelp-error.h"
#include "yelp-settings.h"
#include "yelp-xslt-pager.h"
#include "yelp-debug.h"

#define YELP_NAMESPACE "http://www.gnome.org/yelp/ns"

#define EVENTS_PENDING while (yelp_pager_get_state (pager) <= YELP_PAGER_STATE_RUNNING && gtk_events_pending ()) gtk_main_iteration ();
#define CANCEL_CHECK if (!main_running || yelp_pager_get_state (pager) >= YELP_PAGER_STATE_ERROR) goto done;

extern gboolean main_running;

#define YELP_XSLT_PAGER_GET_PRIVATE(object)(G_TYPE_INSTANCE_GET_PRIVATE ((object), YELP_TYPE_XSLT_PAGER, YelpXsltPagerPriv))

struct _YelpXsltPagerPriv {
    xmlDocPtr               inputDoc;
    xmlDocPtr               outputDoc;
    xsltStylesheetPtr       stylesheet;
    xsltTransformContextPtr transformContext;
};

static void      xslt_pager_class_init   (YelpXsltPagerClass *klass);
static void      xslt_pager_init         (YelpXsltPager      *pager);
static void      xslt_pager_dispose      (GObject            *gobject);

static void      xslt_pager_error        (YelpPager          *pager);
static void      xslt_pager_cancel       (YelpPager          *pager);
static void      xslt_pager_finish       (YelpPager          *pager);

static gboolean  xslt_pager_process      (YelpPager          *pager);

static void      xslt_yelp_document    (xsltTransformContextPtr ctxt,
					xmlNodePtr              node,
					xmlNodePtr              inst,
					xsltStylePreCompPtr     comp);
static void      xslt_yelp_cache       (xsltTransformContextPtr ctxt,
					xmlNodePtr              node,
					xmlNodePtr              inst,
					xsltStylePreCompPtr     comp);

static YelpPagerClass *parent_class;

GType
yelp_xslt_pager_get_type (void)
{
    static GType type = 0;

    if (!type) {
	static const GTypeInfo info = {
	    sizeof (YelpXsltPagerClass),
	    NULL,
	    NULL,
	    (GClassInitFunc) xslt_pager_class_init,
	    NULL,
	    NULL,
	    sizeof (YelpXsltPager),
	    0,
	    (GInstanceInitFunc) xslt_pager_init,
	};
	type = g_type_register_static (YELP_TYPE_PAGER,
				       "YelpXsltPager", 
				       &info, 0);
    }
    return type;
}

static void
xslt_pager_class_init (YelpXsltPagerClass *klass)
{
    GObjectClass   *object_class = G_OBJECT_CLASS (klass);
    YelpPagerClass *pager_class  = YELP_PAGER_CLASS (klass);

    parent_class = g_type_class_peek_parent (klass);

    object_class->dispose     = xslt_pager_dispose;

    pager_class->error        = xslt_pager_error;
    pager_class->cancel       = xslt_pager_cancel;
    pager_class->finish       = xslt_pager_finish;

    pager_class->process      = xslt_pager_process;

    g_type_class_add_private (klass, sizeof (YelpXsltPagerPriv));
}

static void
xslt_pager_init (YelpXsltPager *pager)
{
    pager->priv = YELP_XSLT_PAGER_GET_PRIVATE (pager);
}

static void
xslt_pager_dispose (GObject *object)
{
    YelpXsltPager *pager = YELP_XSLT_PAGER (object);
    YelpXsltPagerPriv *priv = pager->priv;

    if (priv->outputDoc)
	xmlFreeDoc (priv->outputDoc);
    if (priv->stylesheet)
	xsltFreeStylesheet (priv->stylesheet);
    if (priv->transformContext)
	xsltFreeTransformContext (priv->transformContext);
    if (priv->inputDoc)
	xmlFreeDoc (priv->inputDoc);

    G_OBJECT_CLASS (parent_class)->dispose (object);
}

/******************************************************************************/

static gboolean
xslt_pager_process (YelpPager *pager)
{
    YelpXsltPagerPriv  *priv;
    YelpXsltPagerClass *klass;

    YelpDocInfo     *doc_info;
    gchar           *filename = NULL;

    gchar **params = NULL;
    gint    params_i = 0;

    GError *error = NULL;

    debug_print (DB_FUNCTION, "entering\n");
    debug_print (DB_PROFILE, "entering %s", __FUNCTION__);

    g_return_val_if_fail (pager != NULL, FALSE);
    g_return_val_if_fail (YELP_IS_XSLT_PAGER (pager), FALSE);

    priv = YELP_XSLT_PAGER (pager)->priv;
    klass = YELP_XSLT_PAGER_GET_CLASS (pager);

    doc_info = yelp_pager_get_doc_info (pager);

    if (yelp_pager_get_state (pager) >= YELP_PAGER_STATE_ERROR)
	goto done;

    filename = yelp_doc_info_get_filename (doc_info);

    g_object_ref (pager);

    yelp_pager_set_state (pager, YELP_PAGER_STATE_PARSING);
    g_signal_emit_by_name (pager, "parse");

    priv->inputDoc = klass->parse (pager);

    if (priv->inputDoc == NULL) {
	g_set_error (&error, YELP_ERROR, YELP_ERROR_NO_DOC,
		     _("The file ‘%s’ could not be parsed. Either the file "
		       "does not exist, or it is improperly formatted."),
		     filename);
	yelp_pager_error (pager, error);
	goto done;
    }

    EVENTS_PENDING;
    CANCEL_CHECK;

    yelp_pager_set_state (pager, YELP_PAGER_STATE_RUNNING);
    g_signal_emit_by_name (pager, "start");

    EVENTS_PENDING;
    CANCEL_CHECK;

    params = klass->params (pager);

    priv->stylesheet = xsltParseStylesheetFile (BAD_CAST klass->stylesheet);
    if (!priv->stylesheet) {
	g_set_error (&error, YELP_ERROR, YELP_ERROR_PROC,
		     _("The document ‘%s’ could not be processed. The file "
		       "‘%s’ is either missing, or it is not a valid XSLT "
		       "stylesheet."),
		     filename,
		     klass->stylesheet);
	yelp_pager_error (pager, error);
	goto done;
    }

    priv->transformContext = xsltNewTransformContext (priv->stylesheet,
						      priv->inputDoc);
    if (!priv->transformContext) {
	g_set_error (&error, YELP_ERROR, YELP_ERROR_PROC,
		     _("The document ‘%s’ could not be processed. The file "
		       "‘%s’ is either missing, or it is not a valid XSLT "
		       "stylesheet."),
		     filename,
		     klass->stylesheet);
	yelp_pager_error (pager, error);
	goto done;
    }

    priv->transformContext->_private = pager;
    xsltRegisterExtElement (priv->transformContext,
			    BAD_CAST "document",
			    BAD_CAST YELP_NAMESPACE,
			    (xsltTransformFunction) xslt_yelp_document);
    xsltRegisterExtElement (priv->transformContext,
			    BAD_CAST "cache",
			    BAD_CAST YELP_NAMESPACE,
			    (xsltTransformFunction) xslt_yelp_cache);

    EVENTS_PENDING;
    CANCEL_CHECK;

    priv->outputDoc = xsltApplyStylesheetUser (priv->stylesheet,
					       priv->inputDoc,
					       (const char **) params,
					       NULL, NULL,
					       priv->transformContext);
    CANCEL_CHECK;
    g_signal_emit_by_name (pager, "finish");

 done:
    if (params) {
	for (params_i = 0; params[params_i] != NULL; params_i++)
	    if (params_i % 2 == 1)
		g_free (params[params_i]);
	g_free (params);
    }
    g_free (filename);

    if (priv->outputDoc) {
	xmlFreeDoc (priv->outputDoc);
	priv->outputDoc = NULL;
    }
    if (priv->stylesheet) {
	xsltFreeStylesheet (priv->stylesheet);
	priv->stylesheet = NULL;
    }
    if (priv->transformContext) {
	xsltFreeTransformContext (priv->transformContext);
	priv->transformContext = NULL;
    }
    if (priv->inputDoc) {
	xmlFreeDoc (priv->inputDoc);
	priv->inputDoc = NULL;
    }

    g_object_unref (pager);
    
    debug_print (DB_PROFILE, "leaving %s", __FUNCTION__);

    return FALSE;
}

static void
xslt_pager_error (YelpPager *pager)
{
    debug_print (DB_FUNCTION, "entering\n");
    yelp_pager_set_state (pager, YELP_PAGER_STATE_ERROR);
}

static void
xslt_pager_cancel (YelpPager *pager)
{
    YelpXsltPagerPriv *priv = YELP_XSLT_PAGER (pager)->priv;

    debug_print (DB_FUNCTION, "entering\n");

    yelp_pager_set_state (pager, YELP_PAGER_STATE_INVALID);

    if (priv->outputDoc) {
	xmlFreeDoc (priv->outputDoc);
	priv->outputDoc = NULL;
    }
    if (priv->stylesheet) {
	xsltFreeStylesheet (priv->stylesheet);
	priv->stylesheet = NULL;
    }
    if (priv->transformContext) {
	xsltFreeTransformContext (priv->transformContext);
	priv->transformContext = NULL;
    }
    if (priv->inputDoc) {
	xmlFreeDoc (priv->inputDoc);
	priv->inputDoc = NULL;
    }
}

static void
xslt_pager_finish (YelpPager *pager)
{
    debug_print (DB_FUNCTION, "entering\n");
    yelp_pager_set_state (pager, YELP_PAGER_STATE_FINISHED);
}

/** XSLT Extension Elements ***************************************************/

void
xslt_yelp_document (xsltTransformContextPtr ctxt,
		    xmlNodePtr              node,
		    xmlNodePtr              inst,
		    xsltStylePreCompPtr     comp)
{
    GError  *error;
    YelpPage *page;
    xmlChar *page_id = NULL;
    xmlChar *page_title = NULL;
    xmlChar *page_buf;
    gint     buf_size;
    YelpPager *pager;
    xsltStylesheetPtr style = NULL;
    const char *old_outfile;
    xmlDocPtr   new_doc = NULL;
    xmlDocPtr   old_doc;
    xmlNodePtr  old_insert;
    xmlNodePtr  cur;

    if (!ctxt || !node || !inst || !comp)
	return;

    pager = (YelpPager *) ctxt->_private;

    EVENTS_PENDING;
    CANCEL_CHECK;

    debug_print (DB_FUNCTION, "entering\n");

    page_id = xsltEvalAttrValueTemplate (ctxt, inst,
					 (const xmlChar *) "href",
					 NULL);
    if (page_id == NULL) {
	xsltTransformError (ctxt, NULL, inst,
			    _("No href attribute found on yelp:document"));
	/* FIXME: put a real error here */
	error = NULL;
	yelp_pager_error (pager, error);
	goto done;
    }
    debug_print (DB_FUNCTION, "  page_id = \"%s\"\n", page_id);

    old_outfile = ctxt->outputFile;
    old_doc     = ctxt->output;
    old_insert  = ctxt->insert;
    ctxt->outputFile = (const char *) page_id;

    style = xsltNewStylesheet ();
    if (style == NULL) {
	xsltTransformError (ctxt, NULL, inst,
			    _("Out of memory"));
	error = NULL;
	yelp_pager_error (pager, error);
	goto done;
    }

    style->omitXmlDeclaration = TRUE;

    new_doc = xmlNewDoc (BAD_CAST "1.0");
    new_doc->charset = XML_CHAR_ENCODING_UTF8;
    new_doc->dict = ctxt->dict;
    xmlDictReference (new_doc->dict);

    ctxt->output = new_doc;
    ctxt->insert = (xmlNodePtr) new_doc;

    xsltApplyOneTemplate (ctxt, node, inst->children, NULL, NULL);

    CANCEL_CHECK;

    xsltSaveResultToString (&page_buf, &buf_size, new_doc, style);

    ctxt->outputFile = old_outfile;
    ctxt->output     = old_doc;
    ctxt->insert     = old_insert;

    CANCEL_CHECK;

    if (!page_title)
	page_title = BAD_CAST g_strdup ("FIXME");

    page = g_new0 (YelpPage, 1);

    page->page_id  = g_strdup ((gchar *) page_id);
    xmlFree (page_id);
    page_id = NULL;

    page->title    = (gchar *) page_title;
    page->contents = (gchar *) page_buf;

    cur = xmlDocGetRootElement (new_doc);

    if (cur != NULL) {
	for (cur = cur->children; cur; cur = cur->next) {
	    if (!xmlStrcmp (cur->name, (xmlChar *) "head")) {
		for (cur = cur->children; cur; cur = cur->next) {
		    if (!xmlStrcmp (cur->name, (xmlChar *) "link")) {
			xmlChar *rel = xmlGetProp (cur, BAD_CAST "rel");

			if (!xmlStrcmp (rel, (xmlChar *) "previous"))
			    page->prev_id = (gchar *) xmlGetProp (cur, BAD_CAST "href");
			else if (!xmlStrcmp (rel, (xmlChar *) "next"))
			    page->next_id = (gchar *) xmlGetProp (cur, BAD_CAST "href");
			else if (!xmlStrcmp (rel, (xmlChar *) "top"))
			    page->toc_id = (gchar *) xmlGetProp (cur, BAD_CAST "href");

			xmlFree (rel);
		    }
		}
		break;
	    }
	}
    }

    CANCEL_CHECK;

    yelp_pager_add_page (pager, page);
    g_signal_emit_by_name (pager, "page", page->page_id);

    EVENTS_PENDING;
    CANCEL_CHECK;

 done:
    if (new_doc)
	xmlFreeDoc (new_doc);
    if (style)
	xsltFreeStylesheet (style);
}

void
xslt_yelp_cache (xsltTransformContextPtr ctxt,
		 xmlNodePtr              node,
		 xmlNodePtr              inst,
		 xsltStylePreCompPtr     comp)
{
    static GHashTable *keyhash = NULL;
    xmlXPathObjectPtr nodeexpr = NULL;
    xsltStylesheetPtr style = NULL;
    xmlNodePtr  nodeptr;
    xmlNodePtr  tmpnode;
    xmlNodePtr  tmpnode2;
    xmlChar    *keyprop = NULL;
    xmlChar    *nodeprop = NULL;
    const char *old_outfile;
    xmlDocPtr   old_output;
    xmlNodePtr  old_insert;
    xmlDocPtr   new_doc = NULL;
    gchar      *key;
    
    if (!ctxt || !node || !inst || !comp)
	return;

    keyprop = xmlGetProp (inst, BAD_CAST "key");
    if (!keyprop)
	return;
    
    nodeprop = xmlGetProp (inst, BAD_CAST "node");
    if (!nodeprop) {
	xmlFree (keyprop);
        return;
    }

    nodeexpr = xmlXPathEvalExpression (nodeprop, ctxt->xpathCtxt);
    if (!nodeexpr)
	goto done;
    
    /* if we haven't initialized the hash yet, do so */
    if (!keyhash)
	keyhash = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, xmlFree);

    if (nodeexpr->type != XPATH_NODESET) {
	debug_print (DB_WARN, "node attribute [%s] did not evaluate to a nodeset\n", 
	             nodeprop);
	goto done;
    }

    nodeptr = nodeexpr->nodesetval->nodeTab[0];
    if (!nodeptr)
	goto done;

    debug_print (DB_DEBUG, "key=%s node=%s ptr=%p\n", 
                 (gchar *)keyprop, (gchar *)nodeprop, (void *)nodeptr);

    key = g_strdup_printf ("%s%p", keyprop, (void *)nodeptr);
    tmpnode = g_hash_table_lookup (keyhash, key);
    
    if (tmpnode) {
	debug_print (DB_DEBUG, "found cached result\n");
        tmpnode2 = xmlDocCopyNode (tmpnode, tmpnode->doc, 1); 
	xmlAddChild (ctxt->insert, tmpnode2);
	g_free (key);
	goto done;
    }

    old_outfile = ctxt->outputFile;
    old_output  = ctxt->output;
    old_insert  = ctxt->insert;
    ctxt->outputFile = "test";
    
    style = xsltNewStylesheet ();
    if (style == NULL) {
	xsltTransformError (ctxt, NULL, inst, _("Out of memory"));
	g_free (key);
	goto done;
    }

    style->omitXmlDeclaration = TRUE;

    new_doc = xmlNewDoc (BAD_CAST "1.0");
    new_doc->charset = XML_CHAR_ENCODING_UTF8;
    new_doc->dict = ctxt->dict;
    xmlDictReference (new_doc->dict);

    ctxt->output = new_doc;
    ctxt->insert = (xmlNodePtr) new_doc;

    xsltApplyOneTemplate (ctxt, node, inst->children, NULL, NULL);
    
    ctxt->outputFile = old_outfile;
    ctxt->output     = old_output;
    ctxt->insert     = old_insert;

    /* this copy of the node we put in the cache */
    tmpnode = xmlCopyNode (new_doc->children, 1); 
    g_hash_table_insert (keyhash, key, tmpnode);

    /* this copy of the node gets attached to the result tree */
    tmpnode2 = xmlDocCopyNode (tmpnode, tmpnode->doc, 1); 
    xmlAddChild (ctxt->insert, tmpnode2);

    while (gtk_events_pending ())
	gtk_main_iteration ();
    /* FIXME : check for cancel */

done:
    if (keyprop)
	xmlFree (keyprop);
    if (nodeprop)
	xmlFree (nodeprop);
    if (nodeexpr)
	xmlXPathFreeObject (nodeexpr);
    if (new_doc)
	xmlFreeDoc (new_doc);
    if (style)
	xsltFreeStylesheet (style);
}