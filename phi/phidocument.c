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

#include "phi/phipageprivate.h"
#include "phi/phigiostreamprivate.h"

static void phi_document_list_model_iface_init(GListModelInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE(PhiDocument, phi_document, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL, phi_document_list_model_iface_init)
)

static void phi_document_object_finalize(GObject* object) {
	PhiDocument* self = PHI_DOCUMENT(object);
	if (self->document)
		fz_drop_document(self->ctx, self->document);
	if (self->ctx)
		fz_drop_context(self->ctx);
	for (gsize i = 0; i < G_N_ELEMENTS(self->ctx_locks); i++)
		g_mutex_clear(&self->ctx_locks[i]);
	G_OBJECT_CLASS(phi_document_parent_class)->finalize(object);
}

static void phi_document_object_dispose(GObject* object) {
	PhiDocument* self = PHI_DOCUMENT(object);
	if (self->pages) {
		for (gint i = 0; i < self->n_pages; i++)
			if (self->pages[i])
				g_object_unref(self->pages[i]);
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
	
	self->ctx = NULL;
	self->document = NULL;
	self->n_pages = 0;
	self->pages = NULL;
}

static GType phi_document_list_model_get_item_type(GListModel*) {
	return G_TYPE_NONE;
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

PhiDocument* phi_document_new_from_stream(GInputStream* stream, const gchar* magic, GError** error) {
	PhiDocument* self = g_object_new(PHI_TYPE_DOCUMENT, NULL);

	fz_locks_context locks = {
		.user = self,
		.lock = phi_document_ctx_lock_lock,
		.unlock = phi_document_ctx_lock_unlock
	};
	self->ctx = fz_new_context(NULL, &locks, FZ_STORE_DEFAULT);
	fz_register_document_handlers(self->ctx);
	
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

	g_object_unref(stream);
	if (info)
		g_object_unref(info);
	return ret;
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
