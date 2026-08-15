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

#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include "ai-worker.h"
#include "model-download.h"

/* defined by OBS_MODULE_USE_DEFAULT_LOCALE in plugin-main.c */
extern const char *obs_module_text(const char *val);
#define T_(x) obs_module_text(x)

#define SETTING_DELAY "delay"
#define SETTING_BLUR_STRENGTH "blur_strength"
#define SETTING_CONFIDENCE "confidence"
#define SETTING_HOLD "hold_blur_seconds"
#define SETTING_PROCESS_EVERY "process_every"
#define SETTING_FORCE_BLUR "force_blur"
#define SETTING_CLASSES_PRESET "classes_preset"
#define SETTING_CLASSES_GROUP "blur_classes_group"
#define SETTING_PANIC "panic"
#define SETTING_PANIC_SECONDS "panic_seconds"
#define SETTING_BLUR_UNVERIFIED "blur_unverified"
#define SETTING_AI_STATS "ai_stats"
#define SETTING_BLUR_MODE "blur_mode"
#define SETTING_AREA_MARGIN "area_margin"
#define SETTING_AI_DETAIL "ai_detail"
#define SETTING_AI_MODEL "ai_model"
#define SETTING_TEMPORAL "temporal_voting"
#define SETTING_STRONG_HIT "strong_hit"
#define SETTING_STRONG_HIT_SCORE "strong_hit_score"

#define MAX_DELAY_FRAMES 600
#define MAX_AUDIO_BLOCKS 600
#define MAX_SUBMITTED_TS 600
#define NS_PER_SEC 1000000000ULL

#define AI_SIZE 320 /* default cell size; runtime size = f->cell_size */
#define MAX_CELL_SIZE 640
#define AI_STAGES 2
#define MAX_TILES 10 /* up to 3x3 grid + 1 full-frame cell */

/* tile overlap: objects on tile borders stay whole in at least one tile */
#define TILE_OVERLAP 0.12f

/* change detection (A): 32x32 luma thumbnails per tile */
#define THUMB_SIZE 32
#define THUMB_SAME_PCT 2.0   /* below: same frame, skip inference */
#define THUMB_CUT_PCT 15.0   /* above: scene cut, analyze immediately */
#define HEARTBEAT_SUBMITS 30 /* forced inference every N staged atlases */

struct tile_geom {
	int x0, y0, w, h;
};

/* Number of analyzed cells: grid tiles + full-frame cell (grid > 1x1). */
static inline int fullblur_cell_count(int cols, int rows)
{
	int ntiles = cols * rows;
	return ntiles + (ntiles > 1 ? 1 : 0);
}

/* Geometry of cell t in frame coordinates. Cells [0, cols*rows) are grid
 * tiles with TILE_OVERLAP; cell cols*rows is the whole frame. */
static void fullblur_cell_geom(int cols, int rows, int t, uint32_t w,
			       uint32_t h, struct tile_geom *g)
{
	int ntiles = cols * rows;
	if (t >= ntiles) {
		g->x0 = 0;
		g->y0 = 0;
		g->w = (int)w;
		g->h = (int)h;
		return;
	}
	int tc = t % cols;
	int tr = t / cols;
	int tw = (int)(w / (uint32_t)cols);
	int th = (int)(h / (uint32_t)rows);
	int mx = (int)((float)tw * TILE_OVERLAP);
	int my = (int)((float)th * TILE_OVERLAP);
	int x0 = tc * tw - mx;
	int y0 = tr * th - my;
	int x1 = (tc + 1) * tw + mx;
	int y1 = (tr + 1) * th + my;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > (int)w) x1 = (int)w;
	if (y1 > (int)h) y1 = (int)h;
	g->x0 = x0;
	g->y0 = y0;
	g->w = x1 - x0;
	g->h = y1 - y0;
}

/* Position of cell t inside the atlas texture, in pixels (cs = cell size). */
static void fullblur_cell_atlas_pos(int cols, int rows, int t, int cs,
				    int *ax, int *ay)
{
	int ntiles = cols * rows;
	if (t >= ntiles) {
		*ax = cols * cs;
		*ay = 0;
	} else {
		*ax = (t % cols) * cs;
		*ay = (t / cols) * cs;
	}
}

struct fb_box {
	int x, y, w, h;
};

/* NudeNet classes selectable via checkboxes */
struct class_option {
	const char *key;
	const char *class_name;
	const char *label_key;
	bool default_on; /* member of "Recommended" preset */
	bool exposed;    /* member of "Exposed only" preset */
};

static const struct class_option CLASS_OPTIONS[] = {
	{"cls_FEMALE_BREAST_EXPOSED", "FEMALE_BREAST_EXPOSED",
	 "ClassFemaleBreastExposed", true, true},
	{"cls_FEMALE_GENITALIA_EXPOSED", "FEMALE_GENITALIA_EXPOSED",
	 "ClassFemaleGenitaliaExposed", true, true},
	{"cls_MALE_GENITALIA_EXPOSED", "MALE_GENITALIA_EXPOSED",
	 "ClassMaleGenitaliaExposed", true, true},
	{"cls_BUTTOCKS_EXPOSED", "BUTTOCKS_EXPOSED", "ClassButtocksExposed",
	 true, true},
	{"cls_ANUS_EXPOSED", "ANUS_EXPOSED", "ClassAnusExposed", true, true},
	{"cls_BELLY_EXPOSED", "BELLY_EXPOSED", "ClassBellyExposed", true,
	 false},
	{"cls_FEMALE_BREAST_COVERED", "FEMALE_BREAST_COVERED",
	 "ClassFemaleBreastCovered", true, false},
	{"cls_FEMALE_GENITALIA_COVERED", "FEMALE_GENITALIA_COVERED",
	 "ClassFemaleGenitaliaCovered", true, false},
	{"cls_ARMPITS_EXPOSED", "ARMPITS_EXPOSED", "ClassArmpitsExposed",
	 true, false},
	{"cls_MALE_BREAST_EXPOSED", "MALE_BREAST_EXPOSED",
	 "ClassMaleBreastExposed", false, false},
	{"cls_BUTTOCKS_COVERED", "BUTTOCKS_COVERED", "ClassButtocksCovered",
	 false, false},
	{"cls_ANUS_COVERED", "ANUS_COVERED", "ClassAnusCovered", false,
	 false},
	{"cls_BELLY_COVERED", "BELLY_COVERED", "ClassBellyCovered", false,
	 false},
	{"cls_ARMPITS_COVERED", "ARMPITS_COVERED", "ClassArmpitsCovered",
	 false, false},
	{"cls_FEET_EXPOSED", "FEET_EXPOSED", "ClassFeetExposed", false,
	 false},
	{"cls_FEET_COVERED", "FEET_COVERED", "ClassFeetCovered", false,
	 false},
	{"cls_FACE_FEMALE", "FACE_FEMALE", "ClassFaceFemale", false, false},
	{"cls_FACE_MALE", "FACE_MALE", "ClassFaceMale", false, false},
};

#define NUM_CLASS_OPTIONS (sizeof(CLASS_OPTIONS) / sizeof(CLASS_OPTIONS[0]))

struct frame_slot {
	gs_texture_t *tex;
	uint64_t ts;
};

struct audio_block {
	float *data; /* planar: frames * channels */
	uint32_t frames;
	uint64_t ts;
};

struct fullblur_filter {
	obs_source_t *context;

	/* settings */
	double delay_sec;
	int blur_strength;
	double confidence;
	double hold_blur_seconds;
	int process_every;
	bool force_blur;
	int panic_seconds;
	bool blur_unverified;
	uint64_t panic_until;
	bool blur_mode_areas;
	double area_margin;
	int ai_cols;
	int ai_rows;

	/* AI */
	struct ai_worker *ai;
	char ai_model[16];
	char classes_csv[1024];
	int cell_size;
	bool cpu_warned;
	bool ai_create_failed; /* worker init already tried after download */
	gs_texrender_t *ai_texrender;
	gs_stagesurf_t *ai_stage[AI_STAGES];
	uint64_t ai_stage_ts[AI_STAGES];
	bool ai_stage_pending[AI_STAGES];
	int ai_stage_idx;
	uint8_t *ai_rowbuf;
	uint64_t frame_counter;
	uint64_t last_nsfw_time;
	bool ai_warned;

	/* change detection + per-tile result cache */
	uint8_t thumb[MAX_TILES][THUMB_SIZE * THUMB_SIZE];
	bool thumb_valid[MAX_TILES];
	uint32_t tile_hb[MAX_TILES]; /* staged atlases since last submit */
	int tile_verdict[MAX_TILES]; /* -1 unknown, 0 clean, 1 nsfw (voted) */
	struct ai_box tile_boxes[MAX_TILES][AI_MAX_BOXES];
	int tile_box_count[MAX_TILES];
	uint64_t dup_skipped;

	/* temporal voting (K of N over fresh verdicts) */
	int vote_k;
	int vote_n;
	uint8_t tile_hist[MAX_TILES][5];
	uint8_t tile_hist_len[MAX_TILES];
	uint8_t tile_hist_pos[MAX_TILES];
	bool strong_hit;
	double strong_hit_score;

	struct submitted_entry {
		uint64_t ts;
		uint16_t mask; /* bit i = tile i was submitted */
	} submitted[MAX_SUBMITTED_TS];
	size_t submitted_head;
	size_t submitted_used;
	bool ai_stage_pe[AI_STAGES]; /* staged frame was a process_every one */

	/* video delay */
	gs_texrender_t *capture;
	gs_texrender_t *blur_small;
	gs_texrender_t *blur_full;
	struct frame_slot slots[MAX_DELAY_FRAMES];
	size_t slot_head; /* oldest slot index */
	size_t slot_used;
	uint32_t width, height;
	struct fb_box blur_boxes[AI_MAX_BOXES];
	int blur_box_count;

	/* audio delay */
	struct audio_block ablocks[MAX_AUDIO_BLOCKS];
	size_t ablock_head;
	size_t ablock_used;
	uint8_t channels;
	struct obs_audio_data audio_out;
	float *audio_out_buf;
	uint32_t audio_out_capacity;
};

static const char *fullblur_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "FullBlur";
}

static void fullblur_reset_ai_caches(struct fullblur_filter *f)
{
	memset(f->thumb_valid, 0, sizeof(f->thumb_valid));
	for (int t = 0; t < MAX_TILES; t++) {
		f->tile_verdict[t] = -1;
		f->tile_box_count[t] = 0;
		f->tile_hb[t] = 0;
		f->tile_hist_len[t] = 0;
		f->tile_hist_pos[t] = 0;
	}
	f->submitted_head = 0;
	f->submitted_used = 0;
	for (int i = 0; i < AI_STAGES; i++)
		f->ai_stage_pending[i] = false;
}

static void fullblur_create_ai_worker(struct fullblur_filter *f)
{
	if (f->ai) {
		ai_worker_destroy(f->ai);
		f->ai = NULL;
	}

	char *path = model_find_local(f->ai_model);
	if (path) {
		f->ai = ai_worker_create(path, model_input_size(f->ai_model));
		bfree(path);
	}

	if (f->ai) {
		f->cell_size = ai_worker_input_size(f->ai);
		ai_worker_set_confidence(f->ai, f->confidence);
		ai_worker_set_classes(f->ai, f->classes_csv);
		ai_worker_set_verdict_only(f->ai, !f->blur_mode_areas);
		obs_log(LOG_INFO, "AI worker started (model=%s, provider=%s)",
			f->ai_model,
			ai_worker_using_gpu(f->ai) ? "DirectML" : "CPU");
		if (f->cell_size >= 640 && !ai_worker_using_gpu(f->ai) &&
		    !f->cpu_warned) {
			f->cpu_warned = true;
			obs_log(LOG_WARNING,
				"Model %s on CPU will be slow; consider 320n",
				f->ai_model);
		}
	}
}

/* ---------------------------------------------------------------- */
/* video delay                                                       */
/* ---------------------------------------------------------------- */

static void fullblur_video_reset(struct fullblur_filter *f)
{
	for (size_t i = 0; i < MAX_DELAY_FRAMES; i++) {
		if (f->slots[i].tex) {
			gs_texture_destroy(f->slots[i].tex);
			f->slots[i].tex = NULL;
		}
		f->slots[i].ts = 0;
	}
	f->slot_head = 0;
	f->slot_used = 0;
}

static void fullblur_push_frame(struct fullblur_filter *f, gs_texture_t *src,
				uint64_t ts)
{
	if (f->slot_used >= MAX_DELAY_FRAMES) {
		/* queue full: drop oldest (video jump, acceptable) */
		f->slot_head = (f->slot_head + 1) % MAX_DELAY_FRAMES;
		f->slot_used--;
	}

	size_t idx = (f->slot_head + f->slot_used) % MAX_DELAY_FRAMES;
	struct frame_slot *slot = &f->slots[idx];

	if (!slot->tex) {
		slot->tex = gs_texture_create(f->width, f->height, GS_RGBA, 1,
					      NULL, GS_RENDER_TARGET);
		if (!slot->tex)
			return;
	}
	gs_copy_texture(slot->tex, src);
	slot->ts = ts;
	f->slot_used++;
}

/* Returns the newest slot with ts <= cutoff, popping all older slots. */
static struct frame_slot *fullblur_pop_frame(struct fullblur_filter *f,
					     uint64_t cutoff)
{
	struct frame_slot *found = NULL;

	while (f->slot_used > 0) {
		struct frame_slot *head = &f->slots[f->slot_head];
		if (head->ts > cutoff)
			break;
		found = head;
		f->slot_head = (f->slot_head + 1) % MAX_DELAY_FRAMES;
		f->slot_used--;
	}
	return found;
}

static void fullblur_draw_texture(gs_texture_t *tex, uint32_t w, uint32_t h)
{
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	gs_effect_set_texture(image, tex);
	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(tex, 0, w, h);

	gs_blend_state_pop();
}

/* Render a pixelated version of src into f->blur_full (w x h). */
static void fullblur_render_blurred(struct fullblur_filter *f,
				    gs_texture_t *src, uint32_t w, uint32_t h)
{
	int strength = f->blur_strength > 0 ? f->blur_strength : 1;
	uint32_t sw = w / (uint32_t)strength;
	uint32_t sh = h / (uint32_t)strength;
	if (sw < 1)
		sw = 1;
	if (sh < 1)
		sh = 1;

	gs_texrender_reset(f->blur_small);
	if (gs_texrender_begin(f->blur_small, sw, sh)) {
		struct vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)sw, 0.0f, (float)sh, -100.0f, 100.0f);
		fullblur_draw_texture(src, sw, sh);
		gs_texrender_end(f->blur_small);
	}

	gs_texture_t *small = gs_texrender_get_texture(f->blur_small);
	if (!small)
		return;

	gs_texrender_reset(f->blur_full);
	if (gs_texrender_begin(f->blur_full, w, h)) {
		struct vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
		fullblur_draw_texture(small, w, h);
		gs_texrender_end(f->blur_full);
	}
}

/* Draw src normally, then paint blurred subregions over detected boxes. */
static void fullblur_draw_areas(struct fullblur_filter *f, gs_texture_t *src,
				gs_texture_t *blurred, uint32_t w, uint32_t h)
{
	fullblur_draw_texture(src, w, h);

	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	gs_effect_set_texture(image, blurred);
	while (gs_effect_loop(effect, "Draw")) {
		for (int i = 0; i < f->blur_box_count; i++) {
			struct fb_box *b = &f->blur_boxes[i];
			gs_matrix_push();
			gs_matrix_translate3f((float)b->x, (float)b->y, 0.0f);
			gs_draw_sprite_subregion(blurred, 0, (uint32_t)b->x,
						 (uint32_t)b->y, (uint32_t)b->w,
						 (uint32_t)b->h);
			gs_matrix_pop();
		}
	}

	gs_blend_state_pop();
}

/* Render all cells (grid tiles + full-frame cell) into one atlas texture:
 * each cell letterboxed into its own f->cell_size square slot. */
static void fullblur_render_ai_frame(struct fullblur_filter *f,
				     gs_texture_t *src, uint32_t w, uint32_t h)
{
	int cols = f->ai_cols > 0 ? f->ai_cols : 1;
	int rows = f->ai_rows > 0 ? f->ai_rows : 1;
	int cells = fullblur_cell_count(cols, rows);
	int atlas_cols = cols + (cells > cols * rows ? 1 : 0);
	int cs = f->cell_size > 0 ? f->cell_size : AI_SIZE;
	uint32_t atlas_w = (uint32_t)cs * (uint32_t)atlas_cols;
	uint32_t atlas_h = (uint32_t)cs * (uint32_t)rows;
	struct vec4 black;

	gs_texrender_reset(f->ai_texrender);
	if (gs_texrender_begin(f->ai_texrender, atlas_w, atlas_h)) {
		vec4_zero(&black);
		gs_clear(GS_CLEAR_COLOR, &black, 0.0f, 0);
		gs_ortho(0.0f, (float)atlas_w, 0.0f, (float)atlas_h, -100.0f,
			 100.0f);

		gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_eparam_t *image =
			gs_effect_get_param_by_name(effect, "image");

		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		gs_effect_set_texture(image, src);

		while (gs_effect_loop(effect, "Draw")) {
			for (int t = 0; t < cells; t++) {
				struct tile_geom g;
				int ax, ay;
				uint32_t max_side;
				float scale;

				fullblur_cell_geom(cols, rows, t, w, h, &g);
				if (g.w <= 0 || g.h <= 0)
					continue;
				fullblur_cell_atlas_pos(cols, rows, t, cs,
							&ax, &ay);

				max_side = (uint32_t)(g.w > g.h ? g.w : g.h);
				scale = (float)cs / (float)max_side;
				gs_matrix_push();
				gs_matrix_translate3f((float)ax, (float)ay,
						      0.0f);
				gs_matrix_scale3f(scale, scale, 1.0f);
				gs_draw_sprite_subregion(src, 0,
							 (uint32_t)g.x0,
							 (uint32_t)g.y0,
							 (uint32_t)g.w,
							 (uint32_t)g.h);
				gs_matrix_pop();
			}
		}

		gs_blend_state_pop();
		gs_texrender_end(f->ai_texrender);
	}
}

/* Map staged AI atlases from previous render ticks, compare per-tile
 * thumbnails against the previous atlas, and submit only changed tiles. */
static void fullblur_service_ai_stages(struct fullblur_filter *f)
{
	int cols = f->ai_cols > 0 ? f->ai_cols : 1;
	int rows = f->ai_rows > 0 ? f->ai_rows : 1;
	int cells = fullblur_cell_count(cols, rows);
	int cs = f->cell_size > 0 ? f->cell_size : AI_SIZE;

	for (int i = 0; i < AI_STAGES; i++) {
		uint8_t *data;
		uint32_t linesize;

		if (!f->ai_stage_pending[i])
			continue;

		if (!gs_stagesurface_map(f->ai_stage[i], &data, &linesize))
			continue; /* try again next tick */

		uint64_t ts = f->ai_stage_ts[i];
		bool pe_frame = f->ai_stage_pe[i];
		uint16_t submit_mask = 0;

		for (int t = 0; t < cells && t < MAX_TILES; t++) {
			int ax, ay;
			fullblur_cell_atlas_pos(cols, rows, t, cs, &ax, &ay);

			/* 32x32 luma thumbnail from the cell */
			uint8_t cur[THUMB_SIZE * THUMB_SIZE];
			uint32_t diff_sum = 0;
			const int step = cs / THUMB_SIZE;
			for (int ty = 0; ty < THUMB_SIZE; ty++) {
				const uint8_t *row =
					data + (size_t)(ay + ty * step) *
							linesize +
					(size_t)ax * 4;
				for (int tx = 0; tx < THUMB_SIZE; tx++) {
					const uint8_t *px = row +
							    (size_t)tx * step * 4;
					uint8_t luma = (uint8_t)(
						(px[0] * 299 + px[1] * 587 +
						 px[2] * 114) /
						1000);
					cur[ty * THUMB_SIZE + tx] = luma;
					if (f->thumb_valid[t]) {
						int d = (int)luma -
							(int)f->thumb[t]
								     [ty * THUMB_SIZE +
								      tx];
						diff_sum += (uint32_t)(d < 0
									       ? -d
									       : d);
					}
				}
			}

			double diff_pct = 100.0;
			if (f->thumb_valid[t])
				diff_pct = (double)diff_sum /
					   (double)(THUMB_SIZE * THUMB_SIZE) /
					   255.0 * 100.0;

			memcpy(f->thumb[t], cur, sizeof(cur));
			bool first = !f->thumb_valid[t];
			f->thumb_valid[t] = true;

			bool cut = diff_pct >= THUMB_CUT_PCT;
			bool changed = diff_pct >= THUMB_SAME_PCT;
			bool heartbeat = f->tile_hb[t] >= HEARTBEAT_SUBMITS;

			f->tile_hb[t]++;

			bool submit = first || heartbeat || cut ||
				      (changed && pe_frame);

			if (submit) {
				for (int y = 0; y < cs; y++) {
					const uint8_t *src_row =
						data +
						(size_t)(ay + y) * linesize +
						(size_t)ax * 4;
					memcpy(f->ai_rowbuf +
						       (size_t)y * cs * 4,
					       src_row, (size_t)cs * 4);
				}
				ai_worker_submit(f->ai, f->ai_rowbuf, ts,
						 (uint16_t)t);
				submit_mask |= (uint16_t)(1u << t);
				f->tile_hb[t] = 0;
			} else {
				f->dup_skipped++;
			}
		}

		gs_stagesurface_unmap(f->ai_stage[i]);
		f->ai_stage_pending[i] = false;

		if (submit_mask != 0) {
			size_t pos = (f->submitted_head + f->submitted_used) %
				     MAX_SUBMITTED_TS;
			if (f->submitted_used >= MAX_SUBMITTED_TS) {
				f->submitted_head = (f->submitted_head + 1) %
						    MAX_SUBMITTED_TS;
				f->submitted_used--;
				pos = (f->submitted_head + f->submitted_used) %
				      MAX_SUBMITTED_TS;
			}
			f->submitted[pos].ts = ts;
			f->submitted[pos].mask = submit_mask;
			f->submitted_used++;
		}
	}
}

static void fullblur_stage_ai_frame(struct fullblur_filter *f, uint64_t ts,
				    bool pe_frame)
{
	gs_texture_t *atlas = gs_texrender_get_texture(f->ai_texrender);
	int idx = f->ai_stage_idx;
	int cols = f->ai_cols > 0 ? f->ai_cols : 1;
	int rows = f->ai_rows > 0 ? f->ai_rows : 1;
	int cells = fullblur_cell_count(cols, rows);
	int cs = f->cell_size > 0 ? f->cell_size : AI_SIZE;
	uint32_t aw = (uint32_t)cs *
		      (uint32_t)(cols + (cells > cols * rows ? 1 : 0));
	uint32_t ah = (uint32_t)cs * (uint32_t)rows;

	if (!atlas || f->ai_stage_pending[idx])
		return;

	/* Staging surfaces must be created on the graphics thread;
	 * create() runs on the UI thread where gs_* calls silently fail.
	 * Recreate when the atlas size (grid) changes. */
	if (f->ai_stage[idx] &&
	    (gs_stagesurface_get_width(f->ai_stage[idx]) != aw ||
	     gs_stagesurface_get_height(f->ai_stage[idx]) != ah)) {
		gs_stagesurface_destroy(f->ai_stage[idx]);
		f->ai_stage[idx] = NULL;
	}
	if (!f->ai_stage[idx]) {
		f->ai_stage[idx] = gs_stagesurface_create(aw, ah, GS_RGBA);
		if (!f->ai_stage[idx] && !f->ai_warned) {
			f->ai_warned = true;
			obs_log(LOG_WARNING,
				"Failed to create AI staging surface; "
				"auto-blur disabled");
		}
	}
	if (!f->ai_stage[idx])
		return;

	gs_stage_texture(f->ai_stage[idx], atlas);
	f->ai_stage_ts[idx] = ts;
	f->ai_stage_pe[idx] = pe_frame;
	f->ai_stage_pending[idx] = true;
	f->ai_stage_idx = (idx + 1) % AI_STAGES;
}

/* Was tile `t` of frame `ts` actually submitted to the worker? */
static bool fullblur_was_submitted(struct fullblur_filter *f, uint64_t ts,
				   int t)
{
	for (size_t i = 0; i < f->submitted_used; i++) {
		size_t idx = (f->submitted_head + i) % MAX_SUBMITTED_TS;
		if (f->submitted[idx].ts == ts &&
		    (f->submitted[idx].mask & (uint16_t)(1u << t)))
			return true;
	}
	return false;
}

/* Any tile of frame `ts` submitted (for fail-safe)? */
static bool fullblur_frame_submitted(struct fullblur_filter *f, uint64_t ts)
{
	for (size_t i = 0; i < f->submitted_used; i++) {
		size_t idx = (f->submitted_head + i) % MAX_SUBMITTED_TS;
		if (f->submitted[idx].ts == ts)
			return true;
	}
	return false;
}

static void fullblur_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct fullblur_filter *f = (struct fullblur_filter *)data;
	obs_source_t *target = obs_filter_get_target(f->context);
	obs_source_t *parent = obs_filter_get_parent(f->context);

	if (!target || !parent)
		return;

	uint32_t w = obs_source_get_width(target);
	uint32_t h = obs_source_get_height(target);
	if (!w || !h)
		return;

	if (w != f->width || h != f->height) {
		fullblur_video_reset(f);
		f->width = w;
		f->height = h;
		/* geometry changed: cached boxes/verdicts are stale */
		memset(f->thumb_valid, 0, sizeof(f->thumb_valid));
		for (int t = 0; t < MAX_TILES; t++) {
			f->tile_verdict[t] = -1;
			f->tile_box_count[t] = 0;
			f->tile_hist_len[t] = 0;
			f->tile_hist_pos[t] = 0;
		}
	}

	uint64_t now = obs_get_video_frame_time();

	/* 1) capture the parent frame into our scratch texrender */
	gs_texrender_reset(f->capture);
	if (gs_texrender_begin(f->capture, w, h)) {
		uint32_t parent_flags = obs_source_get_output_flags(parent);
		bool custom_draw = (parent_flags & OBS_SOURCE_CUSTOM_DRAW) != 0;
		bool async_src = (parent_flags & OBS_SOURCE_ASYNC) != 0;
		struct vec4 clear;

		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
		gs_blend_state_push();
		gs_blend_function_separate(GS_BLEND_SRCALPHA,
					   GS_BLEND_INVSRCALPHA, GS_BLEND_ONE,
					   GS_BLEND_INVSRCALPHA);

		if (target == parent && !custom_draw && !async_src)
			obs_source_default_render(target);
		else
			obs_source_video_render(target);

		gs_blend_state_pop();
		gs_texrender_end(f->capture);
	}

	gs_texture_t *captured = gs_texrender_get_texture(f->capture);
	if (captured)
		fullblur_push_frame(f, captured, now);

	/* pick up the model once the background download has finished */
	if (!f->ai && !f->ai_create_failed &&
	    model_download_state(f->ai_model) == MODEL_READY) {
		fullblur_reset_ai_caches(f);
		fullblur_create_ai_worker(f);
		if (!f->ai) {
			f->ai_create_failed = true;
			obs_log(LOG_ERROR,
				"AI worker failed to start from downloaded model");
		}
	}

	/* 2) feed the AI worker: atlas every frame (cheap), inference only
	 * for changed tiles (expensive part is skipped for dupes) */
	if (f->ai) {
		fullblur_service_ai_stages(f);

		f->frame_counter++;
		if (captured) {
			bool pe_frame =
				f->frame_counter % (uint64_t)f->process_every ==
				0;
			fullblur_render_ai_frame(f, captured, w, h);
			fullblur_stage_ai_frame(f, now, pe_frame);
		}
	}

	/* 3) output the frame that is at least delay_sec old */
	uint64_t delay_ns = (uint64_t)(f->delay_sec * (double)NS_PER_SEC);
	uint64_t cutoff = (now > delay_ns) ? now - delay_ns : 0;

	struct frame_slot *out = fullblur_pop_frame(f, cutoff);
	if (out && out->tex) {
		bool blur = f->force_blur || now < f->panic_until;
		bool full_frame = f->force_blur || now < f->panic_until;

		if (f->ai) {
			int cols = f->ai_cols > 0 ? f->ai_cols : 1;
			int rows = f->ai_rows > 0 ? f->ai_rows : 1;
			int cells = fullblur_cell_count(cols, rows);
			float margin = (float)f->area_margin;
			if (margin < 1.0f)
				margin = 1.0f;

			bool any_nsfw = false;
			bool any_unknown = false;
			struct fb_box new_boxes[AI_MAX_BOXES];
			int new_box_count = 0;

			for (int t = 0; t < cells && t < MAX_TILES; t++) {
				int v;
				struct ai_box tb[AI_MAX_BOXES];
				int tcount = 0;
				bool fresh_boxes = false;

				if (fullblur_was_submitted(f, out->ts, t)) {
					float max_score = 0.0f;
					v = ai_worker_result_tile(
						f->ai, out->ts, (uint16_t)t,
						tb, &tcount, &max_score);
					if (v != -1) {
						int raw_v = v;

						/* strong hit: confident NSFW
						 * bypasses voting */
						bool bypass =
							f->strong_hit &&
							raw_v == 1 &&
							(max_score >=
							 f->strong_hit_score);

						/* temporal voting: K of N
						 * fresh verdicts */
						if (f->vote_k > 0) {
							uint8_t *h =
								f->tile_hist[t];
							h[f->tile_hist_pos
								  [t]] =
								(uint8_t)
									raw_v;
							f->tile_hist_pos[t] =
								(uint8_t)((
									  f->tile_hist_pos
										  [t] +
									  1) %
									  5);
							if (f->tile_hist_len
								    [t] < 5)
								f->tile_hist_len
									[t]++;

							int window =
								f->vote_n <
										(int)f->tile_hist_len
											[t]
									? f->vote_n
									: (int)f->tile_hist_len
										  [t];
							int votes = 0;
							for (int k2 = 0;
							     k2 < window;
							     k2++) {
								int idx =
									(f->tile_hist_pos
										 [t] +
									 5 -
									 1 -
									 k2) %
									5;
								votes += h[idx];
							}
							int need =
								f->vote_k;
							if (need > window)
								need = 1; /* soft start */
							v = votes >= need
								    ? 1
								    : 0;
						}

						f->tile_verdict[t] = v;
						if (raw_v == 1) {
							memcpy(f->tile_boxes[t],
							       tb,
							       (size_t)tcount *
								       sizeof(tb[0]));
							f->tile_box_count[t] =
								tcount;
							fresh_boxes = true;
						}
					}
				} else {
					/* dupe frame: reuse cached verdict */
					v = f->tile_verdict[t];
				}

				if (v == 1) {
					any_nsfw = true;
					const struct ai_box *src_boxes =
						fresh_boxes ? tb
							    : f->tile_boxes[t];
					int count = fresh_boxes
							    ? tcount
							    : f->tile_box_count[t];

					struct tile_geom g;
					fullblur_cell_geom(cols, rows, t,
							   w, h, &g);
					int max_side =
						g.w > g.h ? g.w : g.h;
					int cs2 = f->cell_size > 0
							  ? f->cell_size
							  : AI_SIZE;
					float inv = max_side > 0
							    ? (float)max_side /
								      (float)cs2
							    : 1.0f;

					for (int bi = 0;
					     bi < count &&
					     new_box_count < AI_MAX_BOXES;
					     bi++) {
						float bw2 = src_boxes[bi].w *
							    inv * margin;
						float bh2 = src_boxes[bi].h *
							    inv * margin;
						float cx = (float)g.x0 +
							   src_boxes[bi].x *
								   inv +
							   src_boxes[bi].w *
								   inv * 0.5f;
						float cy = (float)g.y0 +
							   src_boxes[bi].y *
								   inv +
							   src_boxes[bi].h *
								   inv * 0.5f;
						float bx = cx - bw2 * 0.5f;
						float by = cy - bh2 * 0.5f;
						if (bx < 0) bx = 0;
						if (by < 0) by = 0;
						if (bx + bw2 > (float)w)
							bw2 = (float)w - bx;
						if (by + bh2 > (float)h)
							bh2 = (float)h - by;
						if (bw2 < 4 || bh2 < 4)
							continue;
						new_boxes[new_box_count].x =
							(int)bx;
						new_boxes[new_box_count].y =
							(int)by;
						new_boxes[new_box_count].w =
							(int)bw2;
						new_boxes[new_box_count].h =
							(int)bh2;
						new_box_count++;
					}
				} else if (v == -1) {
					any_unknown = true;
				}
			}

			int verdict = any_nsfw ? 1 : (any_unknown ? -1 : 0);

			uint64_t hold_ns =
				(uint64_t)(f->hold_blur_seconds *
					   (double)NS_PER_SEC);

			if (verdict == 1) {
				f->last_nsfw_time = now;
				memcpy(f->blur_boxes, new_boxes,
				       (size_t)new_box_count *
					       sizeof(new_boxes[0]));
				f->blur_box_count = new_box_count;
			}

			if (verdict == 1 ||
			    (f->last_nsfw_time > 0 &&
			     now - f->last_nsfw_time < hold_ns))
				blur = true;

			/* fail-safe: AI was asked about this frame but has
			 * not answered in time */
			if (!blur && f->blur_unverified && verdict == -1 &&
			    fullblur_frame_submitted(f, out->ts)) {
				blur = true;
				full_frame = true;
			}
		}

		if (!blur)
			f->blur_box_count = 0;

		if (blur) {
			fullblur_render_blurred(f, out->tex, w, h);
			gs_texture_t *blurred =
				gs_texrender_get_texture(f->blur_full);
			if (blurred && !full_frame && f->blur_mode_areas &&
			    f->blur_box_count > 0)
				fullblur_draw_areas(f, out->tex, blurred, w, h);
			else if (blurred)
				fullblur_draw_texture(blurred, w, h);
			else
				fullblur_draw_texture(out->tex, w, h);
		} else {
			fullblur_draw_texture(out->tex, w, h);
		}
	} else {
		/* warmup: no frame old enough yet -> black */
		struct vec4 black;
		vec4_zero(&black);
		gs_clear(GS_CLEAR_COLOR, &black, 0.0f, 0);
	}
}

/* ---------------------------------------------------------------- */
/* audio delay                                                       */
/* ---------------------------------------------------------------- */

static void fullblur_audio_reset(struct fullblur_filter *f)
{
	for (size_t i = 0; i < MAX_AUDIO_BLOCKS; i++) {
		if (f->ablocks[i].data) {
			bfree(f->ablocks[i].data);
			f->ablocks[i].data = NULL;
		}
		f->ablocks[i].frames = 0;
		f->ablocks[i].ts = 0;
	}
	f->ablock_head = 0;
	f->ablock_used = 0;
}

static void fullblur_push_audio(struct fullblur_filter *f,
				const struct obs_audio_data *audio)
{
	if (f->ablock_used >= MAX_AUDIO_BLOCKS) {
		struct audio_block *old = &f->ablocks[f->ablock_head];
		f->ablock_head = (f->ablock_head + 1) % MAX_AUDIO_BLOCKS;
		f->ablock_used--;
		/* slot data buffer is reused below */
		(void)old;
	}

	size_t idx = (f->ablock_head + f->ablock_used) % MAX_AUDIO_BLOCKS;
	struct audio_block *block = &f->ablocks[idx];
	size_t bytes = (size_t)audio->frames * f->channels * sizeof(float);

	if (!block->data || block->frames < audio->frames) {
		if (block->data)
			bfree(block->data);
		block->data = (float *)bmalloc(bytes);
	}

	for (uint8_t ch = 0; ch < f->channels; ch++) {
		const float *src = (const float *)audio->data[ch];
		if (src) {
			memcpy(block->data + (size_t)ch * audio->frames, src,
			       audio->frames * sizeof(float));
		} else {
			memset(block->data + (size_t)ch * audio->frames, 0,
			       audio->frames * sizeof(float));
		}
	}
	block->frames = audio->frames;
	block->ts = audio->timestamp;
	f->ablock_used++;
}

static void fullblur_set_audio_out(struct fullblur_filter *f, const float *data,
				   uint32_t frames, uint64_t ts)
{
	size_t need = (size_t)frames * f->channels;
	if (f->audio_out_capacity < need) {
		if (f->audio_out_buf)
			bfree(f->audio_out_buf);
		f->audio_out_buf = (float *)bmalloc(need * sizeof(float));
		f->audio_out_capacity = (uint32_t)need;
	}

	for (uint8_t ch = 0; ch < f->channels; ch++) {
		float *dst = f->audio_out_buf + (size_t)ch * frames;
		if (data)
			memcpy(dst, data + (size_t)ch * frames,
			       frames * sizeof(float));
		else
			memset(dst, 0, frames * sizeof(float));
		f->audio_out.data[ch] = (uint8_t *)dst;
	}

	f->audio_out.frames = frames;
	f->audio_out.timestamp = ts;
}

static struct obs_audio_data *fullblur_filter_audio(
	void *data, struct obs_audio_data *audio)
{
	struct fullblur_filter *f = (struct fullblur_filter *)data;

	if (!audio || audio->frames == 0)
		return audio;

	uint32_t frames = audio->frames;
	uint64_t ts = audio->timestamp;

	fullblur_push_audio(f, audio);

	uint64_t delay_ns = (uint64_t)(f->delay_sec * (double)NS_PER_SEC);
	struct audio_block *head = &f->ablocks[f->ablock_head];

	if (f->ablock_used > 0 && ts >= head->ts + delay_ns) {
		fullblur_set_audio_out(f, head->data, head->frames, head->ts);
		f->ablock_head = (f->ablock_head + 1) % MAX_AUDIO_BLOCKS;
		f->ablock_used--;
	} else {
		fullblur_set_audio_out(f, NULL, frames, ts);
	}

	return &f->audio_out;
}

/* ---------------------------------------------------------------- */
/* filter boilerplate                                                */
/* ---------------------------------------------------------------- */

static void fullblur_update(void *data, obs_data_t *settings)
{
	struct fullblur_filter *f = (struct fullblur_filter *)data;

	f->delay_sec = obs_data_get_double(settings, SETTING_DELAY);
	f->blur_strength = (int)obs_data_get_int(settings, SETTING_BLUR_STRENGTH);
	f->confidence = obs_data_get_double(settings, SETTING_CONFIDENCE);
	f->hold_blur_seconds = obs_data_get_double(settings, SETTING_HOLD);
	f->process_every = (int)obs_data_get_int(settings, SETTING_PROCESS_EVERY);
	f->force_blur = obs_data_get_bool(settings, SETTING_FORCE_BLUR);
	f->panic_seconds = (int)obs_data_get_int(settings, SETTING_PANIC_SECONDS);
	f->blur_unverified = obs_data_get_bool(settings, SETTING_BLUR_UNVERIFIED);
	f->area_margin = obs_data_get_double(settings, SETTING_AREA_MARGIN);

	const char *mode = obs_data_get_string(settings, SETTING_BLUR_MODE);
	f->blur_mode_areas = mode && strcmp(mode, "areas") == 0;

	const char *detail = obs_data_get_string(settings, SETTING_AI_DETAIL);
	int new_cols, new_rows;
	if (detail && strcmp(detail, "3x3") == 0) {
		new_cols = 3;
		new_rows = 3;
	} else if (detail && strcmp(detail, "2x2") == 0) {
		new_cols = 2;
		new_rows = 2;
	} else {
		new_cols = 1;
		new_rows = 1;
	}
	if (new_cols != f->ai_cols || new_rows != f->ai_rows) {
		/* grid changed: thumbnails and cached verdicts are stale */
		memset(f->thumb_valid, 0, sizeof(f->thumb_valid));
		for (int t = 0; t < MAX_TILES; t++) {
			f->tile_verdict[t] = -1;
			f->tile_box_count[t] = 0;
			f->tile_hb[t] = 0;
			f->tile_hist_len[t] = 0;
			f->tile_hist_pos[t] = 0;
		}
		f->ai_cols = new_cols;
		f->ai_rows = new_rows;
	}

	const char *temporal = obs_data_get_string(settings, SETTING_TEMPORAL);
	if (temporal && strcmp(temporal, "3of5") == 0) {
		f->vote_k = 3;
		f->vote_n = 5;
	} else if (temporal && strcmp(temporal, "off") == 0) {
		f->vote_k = 0;
		f->vote_n = 0;
	} else {
		f->vote_k = 2;
		f->vote_n = 3;
	}

	f->strong_hit = obs_data_get_bool(settings, SETTING_STRONG_HIT);
	f->strong_hit_score =
		obs_data_get_double(settings, SETTING_STRONG_HIT_SCORE);

	f->classes_csv[0] = '\0';
	for (size_t i = 0; i < NUM_CLASS_OPTIONS; i++) {
		if (!obs_data_get_bool(settings, CLASS_OPTIONS[i].key))
			continue;
		if (f->classes_csv[0])
			strncat(f->classes_csv, ",",
				sizeof(f->classes_csv) -
					strlen(f->classes_csv) - 1);
		strncat(f->classes_csv, CLASS_OPTIONS[i].class_name,
			sizeof(f->classes_csv) - strlen(f->classes_csv) - 1);
	}

	/* model switch (also handles initial creation: f->ai_model == "") */
	const char *new_model = obs_data_get_string(settings, SETTING_AI_MODEL);
	if (!model_id_valid(new_model))
		new_model = "320n";
	if (strcmp(new_model, f->ai_model) != 0) {
		strncpy(f->ai_model, new_model, sizeof(f->ai_model) - 1);
		f->ai_model[sizeof(f->ai_model) - 1] = '\0';
		fullblur_reset_ai_caches(f);
		fullblur_create_ai_worker(f);
		if (!f->ai)
			model_download_start(f->ai_model);
	}

	if (f->ai) {
		ai_worker_set_confidence(f->ai, f->confidence);
		ai_worker_set_classes(f->ai, f->classes_csv);
		ai_worker_set_verdict_only(f->ai, !f->blur_mode_areas);
	}
}

static void *fullblur_create(obs_data_t *settings, obs_source_t *context)
{
	struct fullblur_filter *f =
		(struct fullblur_filter *)bzalloc(sizeof(*f));
	f->context = context;
	f->capture = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	f->blur_small = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	f->blur_full = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	f->ai_texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	f->channels = (uint8_t)audio_output_get_channels(obs_get_audio());
	f->ai_rowbuf = (uint8_t *)bmalloc((size_t)MAX_CELL_SIZE *
					  MAX_CELL_SIZE * 4);

	for (int t = 0; t < MAX_TILES; t++)
		f->tile_verdict[t] = -1;

	for (uint8_t ch = 0; ch < MAX_AUDIO_CHANNELS; ch++)
		f->audio_out.data[ch] = NULL;

	/* worker creation (or background model download) happens in update
	 * via the model-switch path */
	fullblur_update(f, settings);
	obs_log(LOG_INFO,
		"FullBlur filter created (delay=%.1fs, %d audio channels)",
		f->delay_sec, f->channels);
	return f;
}

static void fullblur_destroy(void *data)
{
	struct fullblur_filter *f = (struct fullblur_filter *)data;

	if (f->ai)
		ai_worker_destroy(f->ai);

	obs_enter_graphics();
	fullblur_video_reset(f);
	if (f->capture)
		gs_texrender_destroy(f->capture);
	if (f->blur_small)
		gs_texrender_destroy(f->blur_small);
	if (f->blur_full)
		gs_texrender_destroy(f->blur_full);
	if (f->ai_texrender)
		gs_texrender_destroy(f->ai_texrender);
	for (int i = 0; i < AI_STAGES; i++) {
		if (f->ai_stage[i])
			gs_stagesurface_destroy(f->ai_stage[i]);
	}
	obs_leave_graphics();

	fullblur_audio_reset(f);
	if (f->audio_out_buf)
		bfree(f->audio_out_buf);
	if (f->ai_rowbuf)
		bfree(f->ai_rowbuf);
	bfree(f);
	obs_log(LOG_INFO, "FullBlur filter destroyed");
}

static void fullblur_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, SETTING_DELAY, 2.0);
	obs_data_set_default_int(settings, SETTING_BLUR_STRENGTH, 14);
	obs_data_set_default_double(settings, SETTING_CONFIDENCE, 0.2);
	obs_data_set_default_double(settings, SETTING_HOLD, 2.0);
	obs_data_set_default_int(settings, SETTING_PROCESS_EVERY, 2);
	obs_data_set_default_bool(settings, SETTING_FORCE_BLUR, false);
	obs_data_set_default_int(settings, SETTING_PANIC_SECONDS, 10);
	obs_data_set_default_bool(settings, SETTING_BLUR_UNVERIFIED, false);
	obs_data_set_default_string(settings, SETTING_BLUR_MODE, "full");
	obs_data_set_default_double(settings, SETTING_AREA_MARGIN, 1.5);
	obs_data_set_default_string(settings, SETTING_AI_DETAIL, "2x2");
	obs_data_set_default_string(settings, SETTING_AI_MODEL, "320n");
	obs_data_set_default_string(settings, SETTING_TEMPORAL, "2of3");
	obs_data_set_default_bool(settings, SETTING_STRONG_HIT, true);
	obs_data_set_default_double(settings, SETTING_STRONG_HIT_SCORE, 0.5);
	obs_data_set_default_string(settings, SETTING_CLASSES_PRESET,
				    "recommended");
	for (size_t i = 0; i < NUM_CLASS_OPTIONS; i++)
		obs_data_set_default_bool(settings, CLASS_OPTIONS[i].key,
					  CLASS_OPTIONS[i].default_on);
}

static bool fullblur_on_preset(obs_properties_t *props, obs_property_t *prop,
			       obs_data_t *settings)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(prop);

	const char *preset =
		obs_data_get_string(settings, SETTING_CLASSES_PRESET);
	if (!preset || strcmp(preset, "custom") == 0)
		return true;

	for (size_t i = 0; i < NUM_CLASS_OPTIONS; i++) {
		bool on;
		if (strcmp(preset, "all") == 0)
			on = true;
		else if (strcmp(preset, "exposed") == 0)
			on = CLASS_OPTIONS[i].exposed;
		else /* recommended */
			on = CLASS_OPTIONS[i].default_on;
		obs_data_set_bool(settings, CLASS_OPTIONS[i].key, on);
	}
	return true;
}

static bool fullblur_on_class_toggle(obs_properties_t *props,
				     obs_property_t *prop,
				     obs_data_t *settings)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(prop);
	obs_data_set_string(settings, SETTING_CLASSES_PRESET, "custom");
	return true;
}

static bool fullblur_on_panic(obs_properties_t *props, obs_property_t *prop,
			      void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(prop);
	struct fullblur_filter *f = (struct fullblur_filter *)data;
	int seconds = f->panic_seconds > 0 ? f->panic_seconds : 10;
	f->panic_until =
		os_gettime_ns() + (uint64_t)seconds * NS_PER_SEC;
	obs_log(LOG_INFO, "Panic blur for %d seconds", seconds);
	return false;
}

static void fullblur_write_stats(struct fullblur_filter *f,
				 obs_data_t *settings)
{
	char buf[256];
	if (f->ai) {
		uint64_t processed, dropped, nsfw;
		double avg_ms;
		ai_worker_stats(f->ai, &processed, &dropped, &nsfw, &avg_ms);
		snprintf(buf, sizeof(buf), T_("AIStatsFormat"), f->ai_model,
			 ai_worker_using_gpu(f->ai) ? "DirectML" : "CPU",
			 (unsigned long long)processed,
			 (unsigned long long)dropped,
			 (unsigned long long)f->dup_skipped,
			 (unsigned long long)nsfw, avg_ms);
	} else if (model_download_state(f->ai_model) == MODEL_DOWNLOADING) {
		snprintf(buf, sizeof(buf), T_("AIDownloading"),
			 model_download_progress(f->ai_model));
	} else if (model_download_state(f->ai_model) == MODEL_FAILED) {
		snprintf(buf, sizeof(buf), "%s", T_("AIDownloadFailed"));
	} else {
		snprintf(buf, sizeof(buf), "%s", T_("AINotAvailable"));
	}
	obs_data_set_string(settings, SETTING_AI_STATS, buf);
}

static obs_properties_t *fullblur_get_properties(void *data)
{
	struct fullblur_filter *f = (struct fullblur_filter *)data;

	obs_properties_t *props = obs_properties_create();

	obs_properties_add_float_slider(props, SETTING_DELAY,
					T_("Delay"), 0.5, 10.0, 0.1);
	obs_properties_add_int_slider(props, SETTING_BLUR_STRENGTH,
				      T_("BlurStrength"), 2, 40, 1);

	obs_property_t *mode_list = obs_properties_add_list(
		props, SETTING_BLUR_MODE, T_("BlurMode"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(mode_list, T_("FullFrame"), "full");
	obs_property_list_add_string(mode_list, T_("DetectedAreas"),
				     "areas");
	obs_properties_add_float_slider(props, SETTING_AREA_MARGIN,
					T_("AreaMargin"), 1.0, 2.0, 0.1);
	obs_properties_add_float_slider(props, SETTING_CONFIDENCE,
					T_("AIConfidence"), 0.05, 1.0, 0.05);
	obs_properties_add_float_slider(props, SETTING_HOLD,
					T_("HoldBlur"), 0.0, 10.0, 0.1);
	obs_properties_add_int_slider(props, SETTING_PROCESS_EVERY,
				      T_("ProcessEvery"), 1, 10, 1);

	obs_property_t *detail_list = obs_properties_add_list(
		props, SETTING_AI_DETAIL, T_("AIDetail"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(detail_list, T_("SingleTile"),
				     "single");
	obs_property_list_add_string(detail_list, T_("Tiles2x2"), "2x2");
	obs_property_list_add_string(detail_list, T_("Tiles3x3"), "3x3");

	obs_property_t *model_list = obs_properties_add_list(
		props, SETTING_AI_MODEL, T_("AIModel"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(model_list, T_("Model320n"), "320n");
	obs_property_list_add_string(model_list, T_("Model640m"), "640m");

	obs_property_t *temporal_list = obs_properties_add_list(
		props, SETTING_TEMPORAL, T_("TemporalVoting"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(temporal_list, T_("TemporalOff"),
				     "off");
	obs_property_list_add_string(temporal_list, T_("Temporal2of3"),
				     "2of3");
	obs_property_list_add_string(temporal_list, T_("Temporal3of5"),
				     "3of5");

	obs_properties_add_bool(props, SETTING_STRONG_HIT, T_("StrongHit"));
	obs_properties_add_float_slider(props, SETTING_STRONG_HIT_SCORE,
					T_("StrongHitScore"), 0.3, 0.9, 0.05);

	obs_properties_add_bool(props, SETTING_FORCE_BLUR,
				T_("ForceBlur"));

	obs_property_t *preset = obs_properties_add_list(
		props, SETTING_CLASSES_PRESET, T_("ClassesPreset"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(preset, T_("PresetRecommended"),
				     "recommended");
	obs_property_list_add_string(preset, T_("PresetExposed"),
				     "exposed");
	obs_property_list_add_string(preset, T_("PresetAll"), "all");
	obs_property_list_add_string(preset, T_("PresetCustom"), "custom");
	obs_property_set_modified_callback(preset, fullblur_on_preset);

	obs_properties_t *cls_group = obs_properties_create();
	for (size_t i = 0; i < NUM_CLASS_OPTIONS; i++) {
		obs_property_t *b = obs_properties_add_bool(
			cls_group, CLASS_OPTIONS[i].key,
			T_(CLASS_OPTIONS[i].label_key));
		obs_property_set_modified_callback(
			b, fullblur_on_class_toggle);
	}
	obs_properties_add_group(props, SETTING_CLASSES_GROUP,
				 T_("BlurClasses"), OBS_GROUP_NORMAL,
				 cls_group);

	obs_properties_add_bool(props, SETTING_BLUR_UNVERIFIED,
				T_("BlurUnverified"));
	obs_properties_add_int_slider(props, SETTING_PANIC_SECONDS,
				      T_("PanicDuration"), 1, 60, 1);
	obs_properties_add_button(props, SETTING_PANIC, T_("PanicButton"),
				  fullblur_on_panic);

	if (f) {
		obs_data_t *settings = obs_source_get_settings(f->context);
		if (settings) {
			fullblur_write_stats(f, settings);
			obs_data_release(settings);
		}
	}
	obs_properties_add_text(props, SETTING_AI_STATS, T_("AIStats"),
				OBS_TEXT_INFO);

	return props;
}

struct obs_source_info fullblur_filter_info = {
	.id = "fullblur_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO,
	.get_name = fullblur_get_name,
	.create = fullblur_create,
	.destroy = fullblur_destroy,
	.update = fullblur_update,
	.get_defaults = fullblur_get_defaults,
	.get_properties = fullblur_get_properties,
	.video_render = fullblur_video_render,
	.filter_audio = fullblur_filter_audio,
};
