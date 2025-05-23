/**
 * @file opml_source.c  OPML Planet/Blogroll feed list source
 *
 * Copyright (C) 2006-2020 Lars Windolf <lars.windolf@gmx.de>
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
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "node_sources/opml_source.h"

#include <unistd.h>

#include "common.h"
#include "debug.h"
#include "export.h"
#include "node_providers/feed.h"
#include "feedlist.h"
#include "node_providers/folder.h"
#include "node.h"
#include "node_source.h"
#include "xml.h"
#include "ui/icons.h"
#include "ui/liferea_dialog.h"
#include "ui/ui_common.h"

/** default OPML update interval = once a day */
#define OPML_SOURCE_UPDATE_INTERVAL 60*60*24

#define OPML_SOURCE_DEFAULT_TITLE _("New OPML Subscription")

#define SOURCE_ID "fl_opml"

/* OPML subscription list helper functions */

typedef struct mergeCtxt {
	Node		*rootNode;	/**< root node of the OPML feed list source */
	Node		*parent;	/**< currently processed feed list node */
	xmlNodePtr	xmlNode;	/**< currently processed XML node of old OPML doc */
} *mergeCtxtPtr;

static void
opml_source_merge_feed (xmlNodePtr match, gpointer user_data)
{
	mergeCtxtPtr	mergeCtxt = (mergeCtxtPtr)user_data;
	xmlChar		*url, *title;
	gchar		*expr;
	Node		*node = NULL;

	url = xmlGetProp (match, BAD_CAST"xmlUrl");
	title = xmlGetProp (match, BAD_CAST"title");
	if (!title)
		title = xmlGetProp (match, BAD_CAST"description");
	if (!title)
		title = xmlGetProp (match, BAD_CAST"text");
	if (!title && !url)
		return;

	if (url)
		expr = g_strdup_printf ("//outline[@xmlUrl = '%s']", url);
	else
		expr = g_strdup_printf ("//outline[@title = '%s']", title);

	if (!xpath_find (mergeCtxt->xmlNode, expr)) {
		debug (DEBUG_UPDATE, "adding %s (%s)", title, url);
		if (url) {
			node = node_new ("feed");
			node_set_data (node, feed_new ());
			node_set_subscription (node, subscription_new ((gchar *)url, NULL, NULL));
		} else {
			node = node_new ("folder");
		}
		node_set_title (node, (gchar *)title);
		node_set_parent (node, mergeCtxt->parent, -1);
		feedlist_node_imported (node);

		subscription_update (node->subscription, UPDATE_REQUEST_RESET_TITLE | UPDATE_REQUEST_PRIORITY_HIGH);
	}

	/* Recursion if this is a folder */
	if (!url) {
		if (!node) {
			/* if the folder node wasn't created above it
			   must already exist and we search it in the
			   parents children list */
			GSList	*iter = mergeCtxt->parent->children;
			while (iter) {
				if (g_str_equal (title, node_get_title (iter->data)))
					node = iter->data;
				iter = g_slist_next (iter);
			}
		}

		if (node) {
			mergeCtxtPtr mc = g_new0 (struct mergeCtxt, 1);
			mc->rootNode = mergeCtxt->rootNode;
			mc->parent = node;
			mc->xmlNode = mergeCtxt->xmlNode;	// FIXME: must be correct child!
			xpath_foreach_match (match, "./outline", opml_source_merge_feed, (gpointer)mc);
			g_free (mc);
		} else {
			g_print ("opml_source_merge_feed(): bad! bad! very bad!");
		}
	}

	g_free (expr);
	xmlFree (title);
	xmlFree (url);
}

static void
opml_source_check_for_removal (Node *node, gpointer user_data)
{
	gchar		*expr = NULL;

	if (IS_FEED (node)) {
		expr = g_strdup_printf ("//outline[ @xmlUrl='%s' ]", subscription_get_source (node->subscription));
	} else if (IS_FOLDER (node)) {
		node_foreach_child_data (node, opml_source_check_for_removal, user_data);
		expr = g_strdup_printf ("//outline[ (@title='%s') or (@text='%s') or (@description='%s')]", node->title, node->title, node->title);
	} else {
		g_print ("opml_source_check_for_removal(): This should never happen...");
		return;
	}

	if (!xpath_find ((xmlNodePtr)user_data, expr)) {
		debug (DEBUG_UPDATE, "removing %s...", node_get_title (node));
		feedlist_node_removed (node);
	} else {
		debug (DEBUG_UPDATE, "keeping %s...", node_get_title (node));
	}
	g_free (expr);
}

/* OPML subscription type implementation */

static gboolean
opml_subscription_prepare_update_request (subscriptionPtr subscription, UpdateRequest *request)
{
	/* Nothing to do here for simple OPML subscriptions */
	return TRUE;
}

static void
opml_subscription_process_update_result (subscriptionPtr subscription, const UpdateResult * const result, updateFlags flags)
{
	Node		*node = subscription->node;
	mergeCtxtPtr	mergeCtxt;
	xmlDocPtr	doc, oldDoc;
	xmlNodePtr	root, title;

	debug (DEBUG_UPDATE, "OPML download finished data=%d", result->data);

	node->available = FALSE;

	if (result->data) {
		doc = xml_parse (result->data, result->size, NULL);
		if (doc) {
			gchar *filename;

			root = xmlDocGetRootElement (doc);

			/* Go through all existing nodes and remove those whose
			   URLs are not in new feed list. Also removes those URLs
			   from the list that have corresponding existing nodes. */
			node_foreach_child_data (node, opml_source_check_for_removal, (gpointer)root);

			opml_source_export (node);	/* save new feed list tree to disk
			                                   to ensure correct document in
							   next step */

			/* Merge up-to-date OPML feed list. */
			filename = opml_source_get_feedlist (node);
			oldDoc = xmlParseFile (filename);
			g_free (filename);

			mergeCtxt = g_new0 (struct mergeCtxt, 1);
			mergeCtxt->rootNode = node;
			mergeCtxt->parent = node;
			mergeCtxt->xmlNode = xmlDocGetRootElement (oldDoc);

			if (g_str_equal (node_get_title (node), OPML_SOURCE_DEFAULT_TITLE)) {
				title = xpath_find (root, "/opml/head/title");
				if (title) {
					xmlChar *titleStr = xmlNodeListGetString(title->doc, title->xmlChildrenNode, 1);
					if (titleStr) {
						node_set_title (node, (gchar *)titleStr);
						xmlFree (titleStr);
					}
				}
			}

			xpath_foreach_match (root, "/opml/body/outline",
			                     opml_source_merge_feed,
			                     (gpointer)mergeCtxt);
			g_free (mergeCtxt);
			xmlFreeDoc (oldDoc);
			xmlFreeDoc (doc);

			opml_source_export (node);	/* save new feed list tree to disk */

			node->available = TRUE;
		} else {
			g_print ("Cannot parse downloaded OPML document!");
		}
	}

	node_foreach_child_data (node, node_update_subscription, GUINT_TO_POINTER (0));
}

/* subscription type definition */

static struct subscriptionType opmlSubscriptionType = {
	opml_subscription_prepare_update_request,
	opml_subscription_process_update_result
};

/* OPML source type implementation */

static void ui_opml_source_get_source_url (void);

gchar *
opml_source_get_feedlist (Node *node)
{
	return common_create_cache_filename ("plugins", node->id, "opml");
}

void
opml_source_import (Node *node)
{
	gchar	*filename;

	/* We only ship an icon for opml, not for other sources */
	if (g_str_equal (node->source->type->id, SOURCE_ID))
		node->icon = icon_create_from_file ("fl_opml.png");

	debug (DEBUG_CACHE, "starting import of opml source instance (id=%s)", node->id);
	filename = opml_source_get_feedlist (node);
	if (g_file_test (filename, G_FILE_TEST_EXISTS)) {
		import_OPML_feedlist (filename, node, FALSE, TRUE);
	} else {
		g_print ("cannot open \"%s\"", filename);
		node->available = FALSE;
	}
	g_free (filename);

	subscription_set_update_interval (node->subscription, OPML_SOURCE_UPDATE_INTERVAL);

	node->subscription->type = &opmlSubscriptionType;

}

void
opml_source_export (Node *node)
{
	gchar		*filename;

	/* Although the OPML structure won't change, it needs to
	   be saved so that the feed ids are saved to disk after
	   the first import or updates of the source OPML. */

	g_assert (node == node->source->root);

	filename = opml_source_get_feedlist (node);
	export_OPML_feedlist (filename, node, TRUE);
	g_free (filename);

	debug (DEBUG_CACHE, "adding OPML source: title=%s", node_get_title(node));

}

void
opml_source_remove (Node *node)
{
	gchar		*filename;

	/* step 1: delete all child nodes */
	node_foreach_child (node, feedlist_node_removed);
	g_assert (!node->children);

	/* step 2: delete source instance OPML cache file */
	filename = opml_source_get_feedlist (node);
	unlink (filename);
	g_free (filename);
}

static void
opml_source_auto_update (Node *node)
{
	guint64	now;

	now = g_get_real_time();

	/* do daily updates for the feed list and feed updates according to the default interval */
	if (node->subscription->updateState->lastPoll + (guint64)OPML_SOURCE_UPDATE_INTERVAL * (guint64)G_USEC_PER_SEC <= now)
		node_source_update (node);
}

/* node source provider definition */

typedef struct {
	GObject parent_instance;
} OpmlSourceProvider;

typedef struct {
	GObjectClass parent_class;
} OpmlSourceProviderClass;

static void opml_source_provider_init(OpmlSourceProvider *self) { }
static void opml_source_provider_class_init(OpmlSourceProviderClass *klass) { }
static void opml_source_provider_interface_init(NodeSourceProviderInterface *iface) {
	iface->id			= SOURCE_ID;
	iface->name			= N_("Planet, BlogRoll, OPML");
	iface->sourceSubscriptionType	= &opmlSubscriptionType;
	iface->feedSubscriptionType	= feed_get_subscription_type ();
	iface->capabilities		= NODE_SOURCE_CAPABILITY_DYNAMIC_CREATION;
	iface->source_new		= ui_opml_source_get_source_url;
	iface->source_delete		= opml_source_remove;
	iface->source_import		= opml_source_import;
	iface->source_export		= opml_source_export;
	iface->source_get_feedlist	= opml_source_get_feedlist;
	iface->source_auto_update	= opml_source_auto_update;
}

#define OPML_TYPE_SOURCE_PROVIDER (opml_source_provider_get_type())

G_DEFINE_TYPE_WITH_CODE(OpmlSourceProvider, opml_source_provider, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(NODE_TYPE_SOURCE_PROVIDER, opml_source_provider_interface_init))

void
opml_source_register (void)
{
	NodeSourceProviderInterface *iface = NODE_SOURCE_PROVIDER_GET_IFACE (g_object_new (OPML_TYPE_SOURCE_PROVIDER, NULL));
	node_source_type_register (iface);
}

/* GUI callbacks */

static void
on_opml_source_selected (GtkDialog *dialog,
                         gint response_id,
                         gpointer user_data)
{
	if (response_id == GTK_RESPONSE_OK) {
		Node *node = node_new ("node_source");
		node_set_title (node, OPML_SOURCE_DEFAULT_TITLE);
		node_source_new (node, SOURCE_ID, liferea_dialog_entry_get (GTK_WIDGET (dialog), "location_entry"));
		feedlist_node_added (node);
		node_source_update (node);
	}
}

static void
on_opml_file_selected (const gchar *filename, gpointer user_data)
{
	liferea_dialog_entry_set (GTK_WIDGET (user_data), "location_entry", filename);
}

static void
on_opml_file_choose_clicked (GtkButton *button, gpointer user_data)
{
	ui_choose_file (_("Choose OPML File"), _("_Open"), FALSE, on_opml_file_selected, NULL, NULL, "*.opml|*.xml", _("OPML Files"), user_data);
}

static void
ui_opml_source_get_source_url (void)
{
	GtkWidget	*dialog, *button;

	dialog = liferea_dialog_new ("opml_source");
	button = liferea_dialog_lookup (dialog, "select_button");

	g_signal_connect (G_OBJECT (dialog), "response",
			  G_CALLBACK (on_opml_source_selected),
			  NULL);

	g_signal_connect (G_OBJECT (button), "clicked",
	                  G_CALLBACK (on_opml_file_choose_clicked),
			  dialog);
}
