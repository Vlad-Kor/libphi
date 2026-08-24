/*
 * libphi - High performance document renderer for GTK
 * Copyright (C) 2025  Florian "sp1rit" <sp1rit@disoot.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "phi/phidocumentprivate.h"

#include "phi/phigiostreamprivate.h"
#include "phi/phinodedeviceprivate.h"
#include "phi/phipageprivate.h"

#include <math.h>

static void phi_document_list_model_iface_init(GListModelInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE(PhiDocument, phi_document, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL, phi_document_list_model_iface_init)
)

static void phi_document_object_finalize(GObject* object) {
	PhiDocument* self = PHI_DOCUMENT(object);
	if (self->render_document)
		fz_drop_document(self->render_ctx, self->render_document);
	if (self->render_ctx)
		fz_drop_context(self->render_ctx);
	if (self->thumbnail_document)
		fz_drop_document(self->thumbnail_ctx, self->thumbnail_document);
	if (self->thumbnail_ctx)
		fz_drop_context(self->thumbnail_ctx);
	if (self->document)
		fz_drop_document(self->ctx, self->document);
	if (self->ctx)
		fz_drop_context(self->ctx);
	g_clear_object(&self->source_file);
	g_clear_pointer(&self->source_magic, g_free);
	g_mutex_clear(&self->render_lock);
	g_mutex_clear(&self->thumbnail_lock);
	for (gsize i = 0; i < G_N_ELEMENTS(self->ctx_locks); i++)
		g_mutex_clear(&self->ctx_locks[i]);
	G_OBJECT_CLASS(phi_document_parent_class)->finalize(object);
}

static void phi_document_object_dispose(GObject* object) {
	PhiDocument* self = PHI_DOCUMENT(object);
	if (self->pages) {
		for (gint i = 0; i < self->n_pages; i++) {
			if (self->pages[i]) {
				phi_page_detach_document(self->pages[i]);
				g_object_unref(self->pages[i]);
			}
		}
		g_free(self->pages);
		self->pages = NULL;
	}
	G_OBJECT_CLASS(phi_document_parent_class)->dispose(object);
}

static void phi_document_class_init(PhiDocumentClass* klass) {
	GObjectClass* object_class = G_OBJECT_CLASS(klass);
	object_class->finalize = phi_document_object_finalize;
	object_class->dispose = phi_document_object_dispose;
}

static void phi_document_init(PhiDocument* self) {
	for (gsize i = 0; i < G_N_ELEMENTS(self->ctx_locks); i++)
		g_mutex_init(&self->ctx_locks[i]);
	g_mutex_init(&self->thumbnail_lock);
	g_mutex_init(&self->render_lock);
	
	self->ctx = NULL;
	self->document = NULL;
	self->source_file = NULL;
	self->source_magic = NULL;
	self->thumbnail_ctx = NULL;
	self->thumbnail_document = NULL;
	self->render_ctx = NULL;
	self->render_document = NULL;
	self->n_pages = 0;
	self->pages = NULL;
}

static GType phi_document_list_model_get_item_type(GListModel*) {
	return PHI_TYPE_PAGE;
}
static guint phi_document_list_model_get_n_items(GListModel* list) {
	PhiDocument* self = PHI_DOCUMENT(list);
	return self->n_pages;
}
static gpointer phi_document_list_model_get_item(GListModel* list, guint position) {
	PhiDocument* self = PHI_DOCUMENT(list);
	if ((gint)position >= self->n_pages)
		return NULL;
	if (!self->pages[position]) {
		GError* error = NULL;
		PhiPage* page = phi_document_get_page(self, position, &error);
		if (!page) {
			g_critical("Failed to load page: %s", error->message);
			g_error_free(error);
			return NULL;
		}
		return g_object_ref(page);
	}
	return g_object_ref(self->pages[position]);
}
static void phi_document_list_model_iface_init(GListModelInterface *iface) {
	iface->get_item_type = phi_document_list_model_get_item_type;
	iface->get_n_items = phi_document_list_model_get_n_items;
	iface->get_item = phi_document_list_model_get_item;
}

static void phi_document_ctx_lock_lock(void* user, int lock) {
	PhiDocument* self = PHI_DOCUMENT(user);
	g_assert(lock >= 0 && lock < (gint)G_N_ELEMENTS(self->ctx_locks));
	g_mutex_lock(&self->ctx_locks[lock]);
}
static void phi_document_ctx_lock_unlock(void* user, int lock) {
	PhiDocument* self = PHI_DOCUMENT(user);
	g_assert(lock >= 0 && lock < (gint)G_N_ELEMENTS(self->ctx_locks));
	g_mutex_unlock(&self->ctx_locks[lock]);
}

/* Custom warning handler to suppress noisy MuPDF warnings */
static void phi_document_warn_handler(void* user, const char* message) {
	(void)user;
	/* Only log as debug, don't spam the console */
	g_debug("MuPDF: %s", message);
}

/* MuPDF reports recoverable PDF repairs (for example broken xref offsets) via
 * its error stream even when opening ultimately succeeds. Keep those details
 * available under G_MESSAGES_DEBUG without flooding normal application
 * output; actual failures are still returned through GError by our catches. */
static void phi_document_error_handler(void* user, const char* message) {
	(void)user;
	g_debug("MuPDF repair: %s", message);
}

PhiDocument* phi_document_new_from_stream(GInputStream* stream, const gchar* magic, GError** error) {
	PhiDocument* self = g_object_new(PHI_TYPE_DOCUMENT, NULL);

	fz_locks_context locks = {
		.user = self,
		.lock = phi_document_ctx_lock_lock,
		.unlock = phi_document_ctx_lock_unlock
	};
	self->ctx = fz_new_context(NULL, &locks, FZ_STORE_DEFAULT);
	fz_register_document_handlers(self->ctx);
	
	/* Route MuPDF diagnostics through GLib instead of raw stderr. */
	fz_set_error_callback(self->ctx, phi_document_error_handler, NULL);
	fz_set_warning_callback(self->ctx, phi_document_warn_handler, NULL);
	
	// TODO: autodetect magic if it is NULL

	fz_stream* wrapped_stream = NULL;
	fz_try(self->ctx) {
		wrapped_stream = phi_gio_stream_wrap(self->ctx, stream);
		self->document = fz_open_document_with_stream(self->ctx, magic, wrapped_stream);
		self->n_pages = fz_count_pages(self->ctx, self->document);	
	} fz_always(self->ctx) {
		if (wrapped_stream)
			fz_drop_stream(self->ctx, wrapped_stream);
	} fz_catch(self->ctx) {
		gint code;
		const gchar* msg = fz_convert_error(self->ctx, &code);
		g_set_error_literal(error, PHI_MU_ERROR, code, msg);
		g_object_unref(self);
		return NULL;
	}

	self->pages = g_new0(PhiPage*, self->n_pages);
	g_list_model_items_changed(G_LIST_MODEL(self), 0, 0, self->n_pages);
	return self;
}

PhiDocument* phi_document_new_from_file(GFile* file, GError** error) {
	const gchar* content_type = NULL;
	GFileInfo* info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE, G_FILE_QUERY_INFO_NONE, NULL, NULL);
	if (info && g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE))
		content_type = g_file_info_get_content_type(info);

	GFileInputStream* stream = g_file_read(file, NULL, error);
	if (!stream)
		return NULL;

	PhiDocument* ret = phi_document_new_from_stream(G_INPUT_STREAM(stream), content_type, error);
	if (ret) {
		ret->source_file = g_object_ref(file);
		ret->source_magic = g_strdup(content_type);
	}

	g_object_unref(stream);
	if (info)
		g_object_unref(info);
	return ret;
}

gint phi_document_get_n_pages(PhiDocument* self) {
	g_return_val_if_fail(PHI_IS_DOCUMENT(self), 0);
	return self->n_pages;
}

gchar* phi_document_dup_metadata(PhiDocument* self,
		PhiDocumentMetadata metadata) {
	g_return_val_if_fail(PHI_IS_DOCUMENT(self), NULL);

	static const gchar* const keys[] = {
		[PHI_DOCUMENT_METADATA_FORMAT] = FZ_META_FORMAT,
		[PHI_DOCUMENT_METADATA_ENCRYPTION] = FZ_META_ENCRYPTION,
		[PHI_DOCUMENT_METADATA_TITLE] = FZ_META_INFO_TITLE,
		[PHI_DOCUMENT_METADATA_AUTHOR] = FZ_META_INFO_AUTHOR,
		[PHI_DOCUMENT_METADATA_SUBJECT] = FZ_META_INFO_SUBJECT,
		[PHI_DOCUMENT_METADATA_KEYWORDS] = FZ_META_INFO_KEYWORDS,
		[PHI_DOCUMENT_METADATA_CREATOR] = FZ_META_INFO_CREATOR,
		[PHI_DOCUMENT_METADATA_PRODUCER] = FZ_META_INFO_PRODUCER,
		[PHI_DOCUMENT_METADATA_CREATION_DATE] =
			FZ_META_INFO_CREATIONDATE,
		[PHI_DOCUMENT_METADATA_MODIFICATION_DATE] =
			FZ_META_INFO_MODIFICATIONDATE,
	};
	if (metadata < 0 || metadata >= (gint)G_N_ELEMENTS(keys))
		return NULL;

	gchar* value = NULL;
	fz_try(self->ctx) {
		int required = fz_lookup_metadata(self->ctx, self->document,
			keys[metadata], NULL, 0);
		if (required > 0) {
			value = g_malloc(required);
			if (fz_lookup_metadata(self->ctx, self->document, keys[metadata],
					value, required) < 0)
				g_clear_pointer(&value, g_free);
		}
	} fz_catch(self->ctx) {
		g_clear_pointer(&value, g_free);
	}

	if (value && !g_utf8_validate(value, -1, NULL)) {
		gchar* valid = g_utf8_make_valid(value, -1);
		g_free(value);
		value = valid;
	}
	return value;
}

PhiPage* phi_document_get_page(PhiDocument* self, gint pageno, GError** error) {
	g_return_val_if_fail(PHI_IS_DOCUMENT(self), NULL);
	g_return_val_if_fail(pageno >= 0 && pageno < self->n_pages, NULL);

	if (self->pages[pageno])
		return self->pages[pageno];
	
	fz_page* page = NULL;
	fz_try(self->ctx) {
		page = fz_load_page(self->ctx, self->document, pageno);
	} fz_catch(self->ctx) {
		g_set_error_literal(error, PHI_MU_ERROR, fz_caught(self->ctx), fz_caught_message(self->ctx));
		return NULL;
	}
																						  
	PhiPage* cpage = g_object_new(PHI_TYPE_PAGE, NULL);
	cpage->document = self;
	g_object_add_weak_pointer(G_OBJECT(self), (gpointer*)&cpage->document);
	cpage->page = page;
	self->pages[pageno] = cpage; // transfers ownership
	return cpage;
}

cairo_surface_t* phi_document_render_thumbnail(PhiDocument* self, gint pageno,
		gint max_width, gint max_height, GError** error) {
	g_return_val_if_fail(PHI_IS_DOCUMENT(self), NULL);
	g_return_val_if_fail(pageno >= 0 && pageno < self->n_pages, NULL);
	g_return_val_if_fail(max_width > 0 && max_height > 0, NULL);

	if (!self->source_file) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			"Background thumbnails require a file-backed document");
		return NULL;
	}

	gchar* path = g_file_get_path(self->source_file);
	if (!path) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			"Background thumbnails require a local file");
		return NULL;
	}

	cairo_surface_t* surface = NULL;
	fz_page* page = NULL;
	fz_pixmap* pixmap = NULL;

	g_mutex_lock(&self->thumbnail_lock);

	if (!self->thumbnail_ctx) {
		self->thumbnail_ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
		if (!self->thumbnail_ctx) {
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
				"Could not create MuPDF thumbnail context");
			goto unlock;
		}
		fz_register_document_handlers(self->thumbnail_ctx);
		fz_set_error_callback(self->thumbnail_ctx, phi_document_error_handler, NULL);
		fz_set_warning_callback(self->thumbnail_ctx, phi_document_warn_handler, NULL);
	}

	fz_try(self->thumbnail_ctx) {
		if (!self->thumbnail_document)
			self->thumbnail_document = fz_open_document(self->thumbnail_ctx, path);

		page = fz_load_page(self->thumbnail_ctx, self->thumbnail_document, pageno);
		fz_rect bounds = fz_bound_page(self->thumbnail_ctx, page);
		float width = bounds.x1 - bounds.x0;
		float height = bounds.y1 - bounds.y0;
		if (width <= 0 || height <= 0)
			fz_throw(self->thumbnail_ctx, FZ_ERROR_FORMAT,
				"Page has invalid thumbnail bounds");
		float scale = fz_min((float)max_width / width, (float)max_height / height);
		fz_matrix transform = fz_scale(scale, scale);

		pixmap = fz_new_pixmap_from_page(self->thumbnail_ctx, page, transform,
			fz_device_rgb(self->thumbnail_ctx), 0);

		gint pix_width = fz_pixmap_width(self->thumbnail_ctx, pixmap);
		gint pix_height = fz_pixmap_height(self->thumbnail_ctx, pixmap);
		gint src_stride = fz_pixmap_stride(self->thumbnail_ctx, pixmap);
		gint components = fz_pixmap_components(self->thumbnail_ctx, pixmap);
		const guchar* src = fz_pixmap_samples(self->thumbnail_ctx, pixmap);

		surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, pix_width, pix_height);
		if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
			cairo_surface_destroy(surface);
			surface = NULL;
			fz_throw(self->thumbnail_ctx, FZ_ERROR_SYSTEM, "Could not allocate thumbnail surface");
		}

		guchar* dest = cairo_image_surface_get_data(surface);
		gint dest_stride = cairo_image_surface_get_stride(surface);
		for (gint y = 0; y < pix_height; y++) {
			const guchar* src_row = src + y * src_stride;
			guint32* dest_row = (guint32*)(dest + y * dest_stride);
			for (gint x = 0; x < pix_width; x++) {
				const guchar* pixel = src_row + x * components;
				dest_row[x] = ((guint32)pixel[0] << 16) |
					((guint32)pixel[1] << 8) | pixel[2];
			}
		}
		cairo_surface_mark_dirty(surface);
	} fz_always(self->thumbnail_ctx) {
		if (pixmap)
			fz_drop_pixmap(self->thumbnail_ctx, pixmap);
		if (page)
			fz_drop_page(self->thumbnail_ctx, page);
	} fz_catch(self->thumbnail_ctx) {
		if (surface) {
			cairo_surface_destroy(surface);
			surface = NULL;
		}
		g_set_error_literal(error, PHI_MU_ERROR, fz_caught(self->thumbnail_ctx),
			fz_caught_message(self->thumbnail_ctx));
	}

unlock:
	g_mutex_unlock(&self->thumbnail_lock);
	g_free(path);
	return surface;
}

static gboolean phi_document_ensure_render_document(PhiDocument* self,
		GCancellable* cancellable, GError** error) {
	if (self->render_document)
		return TRUE;
	if (!self->source_file) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			"Background page rendering requires a file-backed document");
		return FALSE;
	}

	/* Image nodes retain cloned contexts until their immutable texture bytes
	 * are released, potentially on the GTK thread. MuPDF requires real lock
	 * callbacks whenever a context may be cloned or used across threads. */
	fz_locks_context locks = {
		.user = self,
		.lock = phi_document_ctx_lock_lock,
		.unlock = phi_document_ctx_lock_unlock
	};
	self->render_ctx = fz_new_context(NULL, &locks, FZ_STORE_DEFAULT);
	if (!self->render_ctx) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
			"Could not create MuPDF page-rendering context");
		return FALSE;
	}
	fz_register_document_handlers(self->render_ctx);
	fz_set_error_callback(self->render_ctx, phi_document_error_handler, NULL);
	fz_set_warning_callback(self->render_ctx, phi_document_warn_handler, NULL);

	gchar* path = g_file_get_path(self->source_file);
	GFileInputStream* input = NULL;
	fz_stream* wrapped = NULL;
	if (!path) {
		input = g_file_read(self->source_file, cancellable, error);
		if (!input)
			goto fail;
	}

	fz_try(self->render_ctx) {
		if (path) {
			self->render_document = fz_open_document(self->render_ctx, path);
		} else {
			wrapped = phi_gio_stream_wrap(self->render_ctx,
				G_INPUT_STREAM(input));
			self->render_document = fz_open_document_with_stream(
				self->render_ctx, self->source_magic, wrapped);
		}
	} fz_always(self->render_ctx) {
		if (wrapped)
			fz_drop_stream(self->render_ctx, wrapped);
	} fz_catch(self->render_ctx) {
		g_set_error_literal(error, PHI_MU_ERROR,
			fz_caught(self->render_ctx),
			fz_caught_message(self->render_ctx));
	}

	g_clear_object(&input);
	g_clear_pointer(&path, g_free);
	if (self->render_document)
		return TRUE;

fail:
	g_clear_object(&input);
	g_clear_pointer(&path, g_free);
	if (self->render_ctx) {
		fz_drop_context(self->render_ctx);
		self->render_ctx = NULL;
	}
	return FALSE;
}

static void phi_document_cancel_render_cookie(GCancellable* cancellable,
		gpointer user_data) {
	(void)cancellable;
	fz_cookie* cookie = user_data;
	cookie->abort = 1;
}

GdkTexture* phi_document_render_page_texture(PhiDocument* self, gint pageno,
		gdouble scale, gint tile_x, gint tile_y, gint tile_width,
		gint tile_height, GCancellable* cancellable, GError** error) {
	g_return_val_if_fail(PHI_IS_DOCUMENT(self), NULL);
	g_return_val_if_fail(pageno >= 0 && pageno < self->n_pages, NULL);
	g_return_val_if_fail(isfinite(scale) && scale > 0, NULL);
	g_return_val_if_fail(tile_x >= 0 && tile_y >= 0, NULL);

	GBytes* bytes = NULL;
	fz_page* page = NULL;
	fz_pixmap* pixmap = NULL;
	fz_device* device = NULL;
	fz_cookie cookie = {0};
	gulong cancelled_handler = 0;
	gint pixel_width = 0;
	gint pixel_height = 0;
	gint stride = 0;

	g_mutex_lock(&self->render_lock);
	if ((cancellable &&
		 g_cancellable_set_error_if_cancelled(cancellable, error)) ||
		!phi_document_ensure_render_document(self, cancellable, error))
		goto unlock;

	if (cancellable)
		cancelled_handler = g_cancellable_connect(
			cancellable, G_CALLBACK(phi_document_cancel_render_cookie),
			&cookie, NULL);

	fz_try(self->render_ctx) {
		page = fz_load_page(self->render_ctx, self->render_document, pageno);
		fz_matrix transform = fz_scale((float)scale, (float)scale);
		fz_irect page_box = fz_round_rect(fz_transform_rect(
			fz_bound_page(self->render_ctx, page), transform));
		gint full_width = page_box.x1 - page_box.x0;
		gint full_height = page_box.y1 - page_box.y0;
		if (full_width <= 0 || full_height <= 0)
			fz_throw(self->render_ctx, FZ_ERROR_FORMAT,
				"Page has invalid raster bounds");

		fz_irect render_box = page_box;
		if (tile_width > 0 && tile_height > 0) {
			if (tile_x >= full_width || tile_y >= full_height)
				fz_throw(self->render_ctx, FZ_ERROR_ARGUMENT,
					"Raster tile is outside the page");
			render_box.x0 = page_box.x0 + tile_x;
			render_box.y0 = page_box.y0 + tile_y;
			render_box.x1 = page_box.x0 +
				fz_min(tile_x + tile_width, full_width);
			render_box.y1 = page_box.y0 +
				fz_min(tile_y + tile_height, full_height);
		}

		pixmap = fz_new_pixmap_with_bbox(self->render_ctx,
			fz_device_rgb(self->render_ctx), render_box, NULL, 0);
		fz_clear_pixmap_with_value(self->render_ctx, pixmap, 255);
		device = fz_new_draw_device(self->render_ctx, transform, pixmap);
		fz_run_page(self->render_ctx, page, device, fz_identity, &cookie);
		fz_close_device(self->render_ctx, device);

		if (!cancellable || !g_cancellable_is_cancelled(cancellable)) {
			pixel_width = fz_pixmap_width(self->render_ctx, pixmap);
			pixel_height = fz_pixmap_height(self->render_ctx, pixmap);
			if (fz_pixmap_components(self->render_ctx, pixmap) != 3)
				fz_throw(self->render_ctx, FZ_ERROR_FORMAT,
					"Page raster did not produce RGB pixels");
			stride = fz_pixmap_stride(self->render_ctx, pixmap);
			gsize byte_count = (gsize)stride * pixel_height;
			gpointer copy = g_memdup2(
				fz_pixmap_samples(self->render_ctx, pixmap), byte_count);
			bytes = g_bytes_new_take(copy, byte_count);
		}
	} fz_always(self->render_ctx) {
		if (device)
			fz_drop_device(self->render_ctx, device);
		if (pixmap)
			fz_drop_pixmap(self->render_ctx, pixmap);
		if (page)
			fz_drop_page(self->render_ctx, page);
	} fz_catch(self->render_ctx) {
		if (!cancellable || !g_cancellable_is_cancelled(cancellable))
			g_set_error_literal(error, PHI_MU_ERROR,
				fz_caught(self->render_ctx),
				fz_caught_message(self->render_ctx));
	}

	if (cancelled_handler)
		g_cancellable_disconnect(cancellable, cancelled_handler);
	if (cancellable && g_cancellable_is_cancelled(cancellable)) {
		g_clear_pointer(&bytes, g_bytes_unref);
		g_cancellable_set_error_if_cancelled(cancellable, error);
	}

unlock:
	g_mutex_unlock(&self->render_lock);
	if (!bytes)
		return NULL;

	GdkTexture* texture = gdk_memory_texture_new(
		pixel_width, pixel_height, GDK_MEMORY_R8G8B8, bytes, stride);
	g_bytes_unref(bytes);
	return texture;
}

GskRenderNode* phi_document_render_page_node(PhiDocument* self, gint pageno,
		GCancellable* cancellable, GError** error) {
	g_return_val_if_fail(PHI_IS_DOCUMENT(self), NULL);
	g_return_val_if_fail(pageno >= 0 && pageno < self->n_pages, NULL);

	GskRenderNode* node = NULL;
	fz_page* page = NULL;
	fz_device* device = NULL;
	fz_cookie cookie = {0};
	gulong cancelled_handler = 0;

	g_mutex_lock(&self->render_lock);
	if ((cancellable &&
		 g_cancellable_set_error_if_cancelled(cancellable, error)) ||
		!phi_document_ensure_render_document(self, cancellable, error))
		goto unlock;

	if (cancellable)
		cancelled_handler = g_cancellable_connect(
			cancellable, G_CALLBACK(phi_document_cancel_render_cookie),
			&cookie, NULL);

	fz_try(self->render_ctx) {
		page = fz_load_page(self->render_ctx, self->render_document, pageno);
		device = phi_node_device_new(self->render_ctx, G_OBJECT(self));
		fz_run_page(self->render_ctx, page, device, fz_identity, &cookie);
		if (!cancellable || !g_cancellable_is_cancelled(cancellable))
			node = phi_node_device_pop_root(device);
	} fz_always(self->render_ctx) {
		if (device)
			fz_drop_device(self->render_ctx, device);
		if (page)
			fz_drop_page(self->render_ctx, page);
	} fz_catch(self->render_ctx) {
		if (!cancellable || !g_cancellable_is_cancelled(cancellable))
			g_set_error_literal(error, PHI_MU_ERROR,
				fz_caught(self->render_ctx),
				fz_caught_message(self->render_ctx));
	}

	if (cancelled_handler)
		g_cancellable_disconnect(cancellable, cancelled_handler);
	if (cancellable && g_cancellable_is_cancelled(cancellable)) {
		g_clear_pointer(&node, gsk_render_node_unref);
		g_cancellable_set_error_if_cancelled(cancellable, error);
	}

unlock:
	g_mutex_unlock(&self->render_lock);
	return node;
}

static PhiOutlineItem* phi_outline_convert(fz_context* ctx, fz_outline* outline) {
	if (!outline)
		return NULL;
	
	PhiOutlineItem* item = g_new0(PhiOutlineItem, 1);
	item->title = g_strdup(outline->title);
	item->uri = outline->uri ? g_strdup(outline->uri) : NULL;
	item->page = outline->page.page;
	item->children = phi_outline_convert(ctx, outline->down);
	item->next = phi_outline_convert(ctx, outline->next);
	
	return item;
}

PhiOutlineItem* phi_document_get_outline(PhiDocument* self) {
	g_return_val_if_fail(PHI_IS_DOCUMENT(self), NULL);
	
	fz_outline* outline = NULL;
	PhiOutlineItem* result = NULL;
	
	fz_try(self->ctx) {
		outline = fz_load_outline(self->ctx, self->document);
		result = phi_outline_convert(self->ctx, outline);
	} fz_always(self->ctx) {
		if (outline)
			fz_drop_outline(self->ctx, outline);
	} fz_catch(self->ctx) {
		return NULL;
	}
	
	return result;
}

void phi_outline_item_free(PhiOutlineItem* item) {
	if (!item)
		return;
	
	phi_outline_item_free(item->children);
	phi_outline_item_free(item->next);
	g_free(item->title);
	g_free(item->uri);
	g_free(item);
}

gboolean phi_document_resolve_link(PhiDocument* self, const gchar* uri, PhiLinkDest* dest) {
	g_return_val_if_fail(PHI_IS_DOCUMENT(self), FALSE);
	g_return_val_if_fail(uri != NULL, FALSE);
	g_return_val_if_fail(dest != NULL, FALSE);
	
	fz_link_dest link_dest;
	
	fz_try(self->ctx) {
		link_dest = fz_resolve_link_dest(self->ctx, self->document, uri);
	} fz_catch(self->ctx) {
		return FALSE;
	}
	
	if (link_dest.loc.page < 0)
		return FALSE;
	
	dest->page = link_dest.loc.page;
	dest->x = link_dest.x;
	dest->y = link_dest.y;
	dest->zoom = link_dest.zoom;
	
	return TRUE;
}
