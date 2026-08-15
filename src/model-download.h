/*
FullBlur Filter
Copyright (C) 2026 Sergei Kulishov <rewq95kso@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum model_state {
	MODEL_MISSING = 0, /* no local file, download not started */
	MODEL_DOWNLOADING,
	MODEL_READY, /* file present and verified */
	MODEL_FAILED,
};

/* Known models: "320n" (default, fast) and "640m" (accurate, GPU-only). */
int model_count(void);
const char *model_id_at(int index);
bool model_id_valid(const char *id);

/* Inference input size (square) for the model, e.g. 320 / 640. */
int model_input_size(const char *id);

/* Returns a bfree()-able UTF-8 path to the model onnx if a usable local
 * copy exists (%APPDATA%\fullblur-filter\models first, then the plugin
 * data directory), NULL otherwise. */
char *model_find_local(const char *id);

/* bfree()-able UTF-8 path of the download target inside
 * %APPDATA%\fullblur-filter\models. */
char *model_target_path(const char *id);

/* Starts the background download of the given model if not already
 * running/finished. Cancels a download of a different model in flight.
 * Safe to call multiple times. */
void model_download_start(const char *id);

enum model_state model_download_state(const char *id);

/* 0-100 while MODEL_DOWNLOADING, 100 when MODEL_READY */
int model_download_progress(const char *id);

/* Cancels a running download and joins the thread. Call on module unload. */
void model_download_shutdown(void);

#ifdef __cplusplus
}
#endif
