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
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ai_worker;

#define AI_MAX_BOXES 8

struct ai_box {
	uint16_t x, y, w, h; /* in 320x320 letterbox space of its tile */
	uint16_t tile_idx;   /* 0 for single-tile mode */
};

/* model_path: absolute path to the detector onnx, input_size: inference
 * resolution (square, e.g. 320 / 640). Returns NULL on failure. */
struct ai_worker *ai_worker_create(const char *model_path, int input_size);
void ai_worker_destroy(struct ai_worker *w);

/* Input resolution the worker was created with. */
int ai_worker_input_size(struct ai_worker *w);

void ai_worker_set_confidence(struct ai_worker *w, double confidence);
void ai_worker_set_classes(struct ai_worker *w, const char *classes_csv);

/* verdict_only=true: skip box extraction/sorting (full-frame blur mode). */
void ai_worker_set_verdict_only(struct ai_worker *w, bool verdict_only);

/* rgba320: 320x320 RGBA letterboxed tile, ts: obs video clock (ns) of the
 * source frame, tile_idx: index of the tile within the frame grid. */
void ai_worker_submit(struct ai_worker *w, const uint8_t *rgba320,
		      uint64_t ts, uint16_t tile_idx);

/* Result for an exact (ts, tile_idx) pair.
 * Returns -1 = unknown, 0 = clean, 1 = nsfw.
 * When nsfw, fills up to AI_MAX_BOXES boxes of this tile.
 * max_score (optional): highest class score seen in this tile. */
int ai_worker_result_tile(struct ai_worker *w, uint64_t ts,
			  uint16_t tile_idx, struct ai_box *boxes,
			  int *box_count, float *max_score);

void ai_worker_stats(struct ai_worker *w, uint64_t *processed,
		     uint64_t *dropped, uint64_t *nsfw, double *avg_ms);

/* true if the GPU (DirectML) provider is active, false = CPU */
bool ai_worker_using_gpu(struct ai_worker *w);

#ifdef __cplusplus
}
#endif
