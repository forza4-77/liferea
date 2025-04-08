/**
 * @file feed.c  feed node and subscription type
 *
 * Copyright (C) 2003-2024 Lars Windolf <lars.windolf@gmx.de>
 * Copyright (C) 2004-2006 Nathan J. Conrad <t98502@users.sourceforge.net>
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

#include "node_providers/feed.h"

#include <string.h>

#include "conf.h"
#include "common.h"
#include "db.h"
#include "debug.h"
#include "favicon.h"
#include "feedlist.h"
#include "html.h"
#include "itemlist.h"
#include "metadata.h"
#include "node.h"
#include "update.h"
#include "xml.h"
#include "ui/icons.h"
#include "ui/liferea_shell.h"
#include "ui/subscription_dialog.h"
#include "ui/feed_list_view.h"

feedPtr
feed_new (void)
{
	feedPtr		feed;

	feed = g_new0 (struct feed, 1);

	feed->cacheLimit = CACHE_DEFAULT;
	feed->valid = TRUE;

	return feed;
}

static void
feed_import (Node *node, Node *parent, xmlNodePtr xml, gboolean trusted)
{
	xmlChar		*cacheLimitStr, *title,	*tmp;
	feedPtr		feed = NULL;

	xmlChar	*typeStr = xmlGetProp (xml, BAD_CAST"type");

	feed = feed_new ();
	feed->fhp = feed_type_str_to_fhp ((gchar *)typeStr);

	node_set_data (node, feed);
	node_set_subscription (node, subscription_import (xml, trusted));

	/* Set the feed cache limit */
	cacheLimitStr = xmlGetProp (xml, BAD_CAST "cacheLimit");
	if (cacheLimitStr && !xmlStrcmp (cacheLimitStr, BAD_CAST"unlimited"))
		feed->cacheLimit = CACHE_UNLIMITED;
	else
		feed->cacheLimit = common_parse_long ((gchar *)cacheLimitStr, CACHE_DEFAULT);
	xmlFree (cacheLimitStr);

	/* enclosure auto download flag */
	tmp = xmlGetProp (xml, BAD_CAST"encAutoDownload");
	if (tmp && !xmlStrcmp (tmp, BAD_CAST"true"))
		feed->encAutoDownload = TRUE;
	xmlFree (tmp);

	/* comment feed handling flag */
	tmp = xmlGetProp (xml, BAD_CAST"ignoreComments");
	if (tmp && !xmlStrcmp (tmp, BAD_CAST"true"))
		feed->ignoreComments = TRUE;
	xmlFree (tmp);

	tmp = xmlGetProp (xml, BAD_CAST"markAsRead");
	if (tmp && !xmlStrcmp (tmp, BAD_CAST"true"))
		feed->markAsRead = TRUE;
	xmlFree (tmp);

	tmp = xmlGetProp (xml, BAD_CAST"html5Extract");
	if (tmp && !xmlStrcmp (tmp, BAD_CAST"true"))
		feed->html5Extract = TRUE;
	xmlFree (tmp);

	tmp = xmlGetProp (xml, BAD_CAST"loadItemLink");
	if (tmp && !xmlStrcmp ((xmlChar *)tmp, BAD_CAST"true"))
		feed->loadItemLink = TRUE;
	xmlFree (tmp);

	title = xmlGetProp (xml, BAD_CAST"title");
	if (!title || !xmlStrcmp (title, BAD_CAST"")) {
		if (title)
			xmlFree (title);
		title = xmlGetProp (xml, BAD_CAST"text");
	}

	node_set_title (node, (gchar *)title);
	xmlFree (title);

	if (node->subscription)
		debug (DEBUG_CACHE, "import feed: title=%s source=%s typeStr=%s interval=%d",
		        node_get_title (node),
	        	subscription_get_source (node->subscription),
		        typeStr,
		        subscription_get_update_interval (node->subscription));
	xmlFree (typeStr);
}

static void
feed_export (Node *node, xmlNodePtr xml, gboolean trusted)
{
	feedPtr feed = (feedPtr) node->data;
	gchar *cacheLimit = NULL;

	if (node->subscription)
		subscription_export (node->subscription, xml, trusted);

	if (trusted) {
		if (feed->cacheLimit >= 0)
			cacheLimit = g_strdup_printf ("%d", feed->cacheLimit);
		if (feed->cacheLimit == CACHE_UNLIMITED)
			cacheLimit = g_strdup ("unlimited");
		if (cacheLimit)
			xmlNewProp (xml, BAD_CAST"cacheLimit", BAD_CAST cacheLimit);

		if (feed->encAutoDownload)
			xmlNewProp (xml, BAD_CAST"encAutoDownload", BAD_CAST"true");

		if (feed->ignoreComments)
			xmlNewProp (xml, BAD_CAST"ignoreComments", BAD_CAST"true");

		if (feed->markAsRead)
			xmlNewProp (xml, BAD_CAST"markAsRead", BAD_CAST"true");

		if (feed->html5Extract)
			xmlNewProp (xml, BAD_CAST"html5Extract", BAD_CAST"true");

		if (feed->loadItemLink)
			xmlNewProp (xml, BAD_CAST"loadItemLink", BAD_CAST"true");
	}

	if (node->subscription)
		debug (DEBUG_CACHE, "adding feed: source=%s interval=%d cacheLimit=%s",
		        subscription_get_source (node->subscription),
			subscription_get_update_interval (node->subscription),
		        (cacheLimit != NULL ? cacheLimit : ""));
	g_free (cacheLimit);
}

guint
feed_get_max_item_count (Node *node)
{
	gint	default_max_items;
	feedPtr	feed = (feedPtr)node->data;

	switch (feed->cacheLimit) {
		case CACHE_DEFAULT:
			conf_get_int_value (DEFAULT_MAX_ITEMS, &default_max_items);
			return default_max_items;
			break;
		case CACHE_DISABLE:
		case CACHE_UNLIMITED:
			return G_MAXUINT;
			break;
		default:
			return feed->cacheLimit;
			break;
	}
}

// content scraping

static void
feed_enrich_item_cb (const UpdateResult * const result, gpointer userdata, updateFlags flags) {
	itemPtr item;
	gchar	*article;

	if (!result->data || result->httpstatus >= 400)
		return;

	item = item_load (GPOINTER_TO_UINT (userdata));
	if (!item)
		return;

	article = xhtml_extract_from_string (result->data, result->source);
	if (article) {
		// Enable AMP images by replacing <amg-img> by <img>
		gchar **tmp_split = g_strsplit(article, "<amp-img", 0);
		gchar *tmp = g_strjoinv("<img", tmp_split);
		g_strfreev (tmp_split);
		g_free (article);
		article = tmp;

		metadata_list_set (&(item->metadata), "richContent", article);
		db_item_update (item);
		itemlist_update_item (item);
		g_free (article);
	}
	item_unload (item);
}

/**
 * Checks content of an items source and tries to crawl content
 */
void
feed_enrich_item (subscriptionPtr subscription, itemPtr item)
{
	UpdateRequest *request;

	if (!item->source) {
		debug (DEBUG_PARSING, "Cannot HTML5-enrich item %s because it has no source!", item->title);
		return;
	}

	// Don't enrich twice
	if (NULL != metadata_list_get (item->metadata, "richContent")) {
		debug (DEBUG_PARSING, "Skipping already HTML5 enriched item %s", item->title);
		return;
	}

	// Fetch item->link document and try to parse it as XHTML
	debug (DEBUG_PARSING, "Fetching HTML5 %ld %s : %s", item->id, item->title, item->source);
	request = update_request_new (
		item->source,
		NULL,	// updateState
		subscription->updateOptions	// Pass options of parent feed (e.g. password, proxy...)
	);

	update_job_new (subscription, request, feed_enrich_item_cb, GUINT_TO_POINTER (item->id), UPDATE_REQUEST_NO_FEED);
}

/* implementation of subscription type interface */

static void
feed_process_update_result (subscriptionPtr subscription, const UpdateResult * const result, updateFlags flags)
{
	feedParserCtxtPtr	ctxt;
	Node			*node = subscription->node;


	ctxt = feed_parser_ctxt_new (subscription, result->data, result->size);

	/* try to parse the feed */
	if (!feed_parse (ctxt)) {
		/* No feed found, display an error */
		node->available = FALSE;

	} else if (!ctxt->feed->fhp) {
		/* There's a feed but no handler. This means autodiscovery
		 * found a feed, but we still need to download it.
		 * An update should be in progress that will process it */
	} else {
		/* Feed found, process it */
		itemSetPtr	itemSet;

		node->available = TRUE;

		/* merge the resulting items into the node's item set */
		itemSet = node_get_itemset (node);
		node->newCount = itemset_merge_items (itemSet, ctxt->items, ctxt->feed->valid, ctxt->feed->markAsRead);
		if (node->newCount)
			itemlist_merge_itemset (itemSet);
		itemset_free (itemSet);

		/* restore user defined properties if necessary */
		if ((flags & UPDATE_REQUEST_RESET_TITLE) && ctxt->title)
			node_set_title (node, ctxt->title);
	}

	feed_parser_ctxt_free (ctxt);

	// FIXME: this should not be here, but in subscription.c
	if (FETCH_ERROR_NONE != subscription->error)
		node->available = FALSE;

}

static gboolean
feed_prepare_update_request (subscriptionPtr subscription, UpdateRequest *request)
{
	/* Nothing to do. Feeds require no subscription extra handling. */

	return TRUE;
}

/* implementation of the node type interface */

static itemSetPtr
feed_load (Node *node)
{
	return db_itemset_load(node->id);
}

static void
feed_save (Node *node)
{
	/* Nothing to do. Feeds do not have any UI states */
}

static void
feed_update_counters (Node *node)
{
	node->itemCount = db_itemset_get_item_count (node->id);
	node->unreadCount = db_itemset_get_unread_count (node->id);
}

static void
feed_remove (Node *node)
{
	feed_list_view_remove_node (node);

	favicon_remove_from_cache (node->id);
	db_subscription_remove (node->id);
}

static gboolean
feed_add (void)
{
	subscription_dialog_new ();
	return TRUE;
}

static void
feed_properties (Node *node)
{
	subscription_prop_dialog_new (node->subscription);
}

static void
feed_free (Node *node)
{
	feedPtr	feed = (feedPtr)node->data;

	if (feed->parseErrors)
		g_string_free (feed->parseErrors, TRUE);
	g_free (feed);
}

subscriptionTypePtr
feed_get_subscription_type (void)
{
	static struct subscriptionType sti = {
		feed_prepare_update_request,
		feed_process_update_result
	};

	return &sti;
}

nodeProviderPtr
feed_get_provider (void)
{
	static struct nodeProvider nti = {
		NODE_CAPABILITY_SHOW_UNREAD_COUNT |
		NODE_CAPABILITY_UPDATE |
		NODE_CAPABILITY_UPDATE_FAVICON |
		NODE_CAPABILITY_EXPORT |
		NODE_CAPABILITY_EXPORT_ITEMS,
		"feed",		/* not used, feed format ids are used instead */
		ICON_DEFAULT,
		feed_import,
		feed_export,
		feed_load,
		feed_save,
		feed_update_counters,
		feed_remove,
		feed_add,
		feed_properties,
		feed_free
	};

	return &nti;
}
