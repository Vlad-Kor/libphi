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

#include "phi/phigiostreamprivate.h"

G_NORETURN
static void phi_gio_stream_throw_gerror(fz_context* ctx, GError* error) {
	gchar msg[sizeof(ctx->error.message)];
	g_strlcpy(msg, error->message, sizeof msg);
	g_error_free(error);
	fz_throw(ctx, FZ_ERROR_LIBRARY, "%s", msg);
}

typedef struct {
	GInputStream *stream;
	guchar buffer[8192];
} PhiGioStreamState;

static void phi_gio_stream_drop(fz_context*, PhiGioStreamState* state) {
	g_object_unref(state->stream);
	g_free(state);
}

static int phi_gio_stream_next(fz_context* ctx, fz_stream* stream, G_GNUC_UNUSED size_t max) {
	PhiGioStreamState* state = (PhiGioStreamState*)stream->state;
	GError* err = NULL;
	gssize len = g_input_stream_read(state->stream, state->buffer, sizeof state->buffer, NULL, &err);
	if (len < 0)
		phi_gio_stream_throw_gerror(ctx, err);
	
	if (len == 0)
		return -1;

	stream->rp = state->buffer;
	stream->wp = &state->buffer[len];
	stream->pos += len;
	return *stream->rp++;
}

static void phi_gio_stream_seek(fz_context* ctx, fz_stream* stream, int64_t offset, int whence) {
	PhiGioStreamState* state = (PhiGioStreamState*)stream->state;

	GSeekType type;
	switch (whence) {
		case SEEK_SET:
			type = G_SEEK_SET;
			break;
		case SEEK_END:
			type = G_SEEK_END;
			break;
		case SEEK_CUR:
		default:
			type = G_SEEK_CUR;
			break;
	}

	GError* err = NULL;
	if (!g_seekable_seek(G_SEEKABLE(state->stream), offset, type, NULL, &err))
		phi_gio_stream_throw_gerror(ctx, err);
	stream->pos = g_seekable_tell (G_SEEKABLE(state->stream));
	stream->rp = state->buffer;
	stream->wp = state->buffer;
}

fz_stream* phi_gio_stream_wrap(fz_context* ctx, GInputStream* stream) {
	g_return_val_if_fail(G_IS_INPUT_STREAM(stream) && G_IS_SEEKABLE(stream), NULL);
	g_return_val_if_fail(g_seekable_can_seek(G_SEEKABLE(stream)), NULL);

	PhiGioStreamState* state = g_new0(PhiGioStreamState, 1);
	state->stream = g_object_ref(stream);

	fz_stream* ret = fz_new_stream(ctx, state, phi_gio_stream_next, (fz_stream_drop_fn*)phi_gio_stream_drop);
	ret->seek = phi_gio_stream_seek;
	return ret;
}
