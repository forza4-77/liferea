/*
 * @file htmlview.c  htmlview reader mode switching and CSS handling
 *
 * Copyright (C) 2021 Lars Windolf <lars.windolf@gmx.de>
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

/**
 * loadContent() will be run on each internal / Readability.js rendering
 */
function loadContent(readerEnabled, content) {
	if (false == readerEnabled) {
		if (document.location.href === 'liferea://') {
			console.log('[liferea] reader mode is off');
			document.body.innerHTML = decodeURIComponent(content);
		} else {
			console.log('[liferea] reader mode off for website');
		}
	}
	if (true == readerEnabled) {
		try {
			console.log('[liferea] enabling reader mode...');
			var documentClone = document.cloneNode(true);

			// When we are internally browsing than we need basic
			// structure to insert Reader mode content
			if(document.location.href !== 'liferea://') {
				content = document.documentElement.innerHTML;
				document.body.innerHTML = '<div id=\"content\"></div>';
			} else {
				// Add all content in shadow DOM and split decoration from content
				// only pass the content to Readability.js
				content = decodeURIComponent(content);
				documentClone.body.innerHTML = content;
				documentClone.getElementById('content').innerHTML = '';
				document.body.innerHTML = documentClone.body.innerHTML;
				documentClone.body.innerHTML = content;
				documentClone.body.innerHTML = documentClone.getElementById('content').innerHTML;
			}

			if (!isProbablyReaderable(documentClone)) {
				document.documentElement.innerHTML = content;
				return;
			}

			// Show the results
			var article = new Readability(documentClone).parse();
			if (article)
				document.getElementById('content').innerHTML = article.content;

			if(document.location.href !== 'liferea://') {
				// Kill all foreign styles
				var links = document.querySelectorAll('link');
				for (var l of links) {
					l.parentNode.removeChild(l);
				}
				var styles = document.querySelectorAll('style');
				for (var s of styles) {
					s.parentNode.removeChild(s);
				}

				// FIXME: Add our header
			}
		} catch(e) {
			console.log('[liferea] reader mode failed: '+e);
			document.documentElement.innerHTML = content;
		}
	}
}
