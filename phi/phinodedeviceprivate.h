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

#ifndef __PHINODEDEVICEPRIVATE_H__
#define __PHINODEDEVICEPRIVATE_H__

#include <gsk/gsk.h>
#include <mupdf/fitz.h>

fz_device* phi_node_device_new(fz_context* ctx);

GskRenderNode* phi_node_device_pop_root(fz_device *self);

#endif // __PHINODEDEVICEPRIVATE_H__
