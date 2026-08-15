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

#include "ai-worker.h"

#include <obs-module.h>
#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>

namespace {

constexpr int NUM_CLASSES = 18;
constexpr size_t MAX_RESULTS = 600;
constexpr size_t MAX_QUEUE = 32;

const char *LABELS[NUM_CLASSES] = {
	"FEMALE_GENITALIA_COVERED", "FACE_FEMALE",
	"BUTTOCKS_EXPOSED",         "FEMALE_BREAST_EXPOSED",
	"FEMALE_GENITALIA_EXPOSED", "MALE_BREAST_EXPOSED",
	"ANUS_EXPOSED",             "FEET_EXPOSED",
	"BELLY_COVERED",            "FEET_COVERED",
	"ARMPITS_COVERED",          "ARMPITS_EXPOSED",
	"FACE_MALE",                "BELLY_EXPOSED",
	"MALE_GENITALIA_EXPOSED",   "ANUS_COVERED",
	"FEMALE_BREAST_COVERED",    "BUTTOCKS_COVERED",
};

using frame_key = std::pair<uint64_t, uint16_t>; /* (ts, tile_idx) */

struct queue_item {
	std::vector<uint8_t> pixels;
	uint64_t ts = 0;
	uint16_t tile = 0;
};

struct result_entry {
	uint8_t verdict = 0;
	float max_score = 0.0f;
	std::vector<ai_box> boxes; /* tagged with tile_idx */
};

} // namespace

struct ai_worker {
	Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "fullblur"};
	Ort::Session session{nullptr};
	Ort::AllocatorWithDefaultOptions alloc;
	std::string input_name;
	std::string output_name;
	bool gpu = false;
	int in_size = 320;

	std::thread thread;
	std::mutex m;
	std::condition_variable cv;
	bool stop = false;

	std::deque<queue_item> queue;

	std::map<frame_key, result_entry> results;
	std::deque<frame_key> result_order;

	std::atomic<double> confidence{0.2};
	std::atomic<bool> verdict_only{false};
	bool class_mask[NUM_CLASSES] = {false};

	uint64_t processed = 0;
	uint64_t dropped = 0;
	uint64_t nsfw = 0;
	double total_ms = 0.0;

	ai_worker(const wchar_t *model_path, int input_size)
	{
		in_size = input_size > 0 ? input_size : 320;

		Ort::SessionOptions so;
		so.SetIntraOpNumThreads(2);
		so.SetGraphOptimizationLevel(
			GraphOptimizationLevel::ORT_ENABLE_ALL);

		/* DirectML via GetProcAddress: no load-time dependency on the
		 * DML entry point, so the plugin still loads if it is absent. */
		HMODULE ort_mod = GetModuleHandleW(L"onnxruntime.dll");
		if (ort_mod) {
			using AppendDmlFn = OrtStatus *(*)(
					OrtSessionOptions *, int);
			auto append_dml = reinterpret_cast<AppendDmlFn>(
				GetProcAddress(
					ort_mod,
					"OrtSessionOptionsAppendExecutionProvider_DML"));
			if (append_dml) {
				OrtStatus *st = append_dml(so, 0);
				if (st) {
					Ort::GetApi().ReleaseStatus(st);
				} else {
					gpu = true;
				}
			}
		}

		session = Ort::Session(env, model_path, so);
		input_name =
			session.GetInputNameAllocated(0, alloc).get();
		output_name =
			session.GetOutputNameAllocated(0, alloc).get();

		thread = std::thread([this] { run(); });
	}

	~ai_worker()
	{
		{
			std::lock_guard<std::mutex> lock(m);
			stop = true;
		}
		cv.notify_all();
		if (thread.joinable())
			thread.join();
	}

	void set_classes(const char *csv)
	{
		std::lock_guard<std::mutex> lock(m);
		for (int i = 0; i < NUM_CLASSES; i++)
			class_mask[i] = false;

		std::string s(csv ? csv : "");
		size_t pos = 0;
		while (pos < s.size()) {
			size_t comma = s.find(',', pos);
			std::string cls = s.substr(
				pos, comma == std::string::npos
					     ? std::string::npos
					     : comma - pos);
			size_t a = cls.find_first_not_of(" \t\r\n");
			size_t b = cls.find_last_not_of(" \t\r\n");
			if (a != std::string::npos)
				cls = cls.substr(a, b - a + 1);
			else
				cls.clear();

			for (int i = 0; i < NUM_CLASSES; i++) {
				if (cls == LABELS[i]) {
					class_mask[i] = true;
					break;
				}
			}
			if (comma == std::string::npos)
				break;
			pos = comma + 1;
		}
	}

	void run()
	{
		const int IS = in_size;
		std::vector<float> input((size_t)3 * IS * IS);

		Ort::MemoryInfo mem_info = Ort::MemoryInfo::CreateCpu(
			OrtArenaAllocator, OrtMemTypeDefault);
		const int64_t dims[4] = {1, 3, IS, IS};
		const char *input_names[] = {input_name.c_str()};
		const char *output_names[] = {output_name.c_str()};

		while (true) {
			queue_item item;
			{
				std::unique_lock<std::mutex> lock(m);
				cv.wait(lock, [this] {
					return stop || !queue.empty();
				});
				if (stop)
					return;
				item = std::move(queue.front());
				queue.pop_front();
			}

			auto t0 = std::chrono::steady_clock::now();

			/* RGBA -> RGB planar float32, /255 */
			const size_t px = (size_t)IS * IS;
			for (int c = 0; c < 3; c++) {
				float *dst = input.data() + (size_t)c * px;
				const uint8_t *src = item.pixels.data() + c;
				for (size_t i = 0; i < px; i++)
					dst[i] = (float)src[i * 4] /
						 255.0f;
			}

			result_entry entry;
			std::vector<std::pair<float, ai_box>> hits;
			try {
				Ort::Value tensor = Ort::Value::CreateTensor<float>(
					mem_info, input.data(), input.size(),
					dims, 4);

				auto outputs = session.Run(
					Ort::RunOptions{nullptr},
					input_names, &tensor, 1,
					output_names, 1);

				/* output shape is dynamic: [1, 4+18, N] */
				auto out_shape = outputs[0]
							 .GetTensorTypeAndShapeInfo()
							 .GetShape();
				if (out_shape.size() != 3 ||
				    out_shape[1] != 4 + NUM_CLASSES) {
					blog(LOG_ERROR,
					     "[fullblur-filter] unexpected model output shape");
					throw std::runtime_error("bad shape");
				}
				const int num_cand = (int)out_shape[2];

				const float *out =
					outputs[0].GetTensorData<float>();

				double thr = confidence.load();
				if (thr < 0.15)
					thr = 0.15;

				bool local_mask[NUM_CLASSES];
				{
					std::lock_guard<std::mutex> lock(m);
					memcpy(local_mask, class_mask,
					       sizeof(local_mask));
				}

				const bool boxes_needed = !verdict_only.load();

				for (int i = 0; i < num_cand; i++) {
					float best_score = 0.0f;
					int best_cls = -1;
					for (int cls = 0; cls < NUM_CLASSES;
					     cls++) {
						if (!local_mask[cls])
							continue;
						float score =
							out[(4 + cls) *
								    num_cand +
							    i];
						if (score > best_score) {
							best_score = score;
							best_cls = cls;
						}
					}
					if (best_cls < 0)
						continue;
					if (best_score > entry.max_score)
						entry.max_score = best_score;
					if (best_score < thr)
						continue;

					entry.verdict = 1;

					if (!boxes_needed)
						continue; /* scan on for max score */

					float cx = out[0 * num_cand + i];
					float cy = out[1 * num_cand + i];
					float bw = out[2 * num_cand + i];
					float bh = out[3 * num_cand + i];
					int x = (int)(cx - bw / 2.0f);
					int y = (int)(cy - bh / 2.0f);
					int ww = (int)bw;
					int hh = (int)bh;
					if (x < 0) { ww += x; x = 0; }
					if (y < 0) { hh += y; y = 0; }
					if (x + ww > IS) ww = IS - x;
					if (y + hh > IS) hh = IS - y;
					if (ww <= 0 || hh <= 0)
						continue;

					ai_box box{(uint16_t)x, (uint16_t)y,
						   (uint16_t)ww, (uint16_t)hh,
						   item.tile};
					hits.emplace_back(best_score, box);
				}

				if (hits.size() > AI_MAX_BOXES) {
					std::partial_sort(
						hits.begin(),
						hits.begin() + AI_MAX_BOXES,
						hits.end(),
						[](const auto &a, const auto &b) {
							return a.first > b.first;
						});
					hits.resize(AI_MAX_BOXES);
				}
				entry.boxes.reserve(hits.size());
				for (const auto &h : hits)
					entry.boxes.push_back(h.second);
			} catch (const std::exception &) {
				entry.verdict = 0;
				entry.boxes.clear();
			}

			auto t1 = std::chrono::steady_clock::now();
			double ms = std::chrono::duration<double, std::milli>(
					    t1 - t0)
					    .count();

			{
				std::lock_guard<std::mutex> lock(m);
				frame_key key{item.ts, item.tile};
				results[key] = std::move(entry);
				result_order.push_back(key);
				if (result_order.size() > MAX_RESULTS) {
					results.erase(result_order.front());
					result_order.pop_front();
				}
				processed++;
				total_ms += ms;
				if (results[key].verdict == 1)
					nsfw++;
			}
		}
	}
};

extern "C" {

struct ai_worker *ai_worker_create(const char *model_path, int input_size)
{
	try {
		wchar_t wpath[MAX_PATH * 2];
		int n = MultiByteToWideChar(CP_UTF8, 0, model_path, -1,
					    wpath, (int)std::size(wpath));
		if (n <= 0) {
			blog(LOG_ERROR,
			     "[fullblur-filter] bad model path encoding");
			return nullptr;
		}
		return new ai_worker(wpath, input_size);
	} catch (const std::exception &e) {
		blog(LOG_ERROR, "[fullblur-filter] AI init failed: %s",
		     e.what());
		return nullptr;
	}
}

int ai_worker_input_size(struct ai_worker *w)
{
	return w ? w->in_size : 320;
}

void ai_worker_destroy(struct ai_worker *w)
{
	delete w;
}

void ai_worker_set_confidence(struct ai_worker *w, double confidence)
{
	if (w)
		w->confidence = confidence;
}

void ai_worker_set_classes(struct ai_worker *w, const char *classes_csv)
{
	if (w)
		w->set_classes(classes_csv);
}

void ai_worker_submit(struct ai_worker *w, const uint8_t *rgba320,
		      uint64_t ts, uint16_t tile_idx)
{
	if (!w)
		return;
	{
		std::lock_guard<std::mutex> lock(w->m);
		if (w->queue.size() >= MAX_QUEUE) {
			w->queue.pop_front();
			w->dropped++;
		}
		queue_item item;
		size_t bytes = (size_t)w->in_size * w->in_size * 4;
		item.pixels.assign(rgba320, rgba320 + bytes);
		item.ts = ts;
		item.tile = tile_idx;
		w->queue.push_back(std::move(item));
	}
	w->cv.notify_one();
}

void ai_worker_set_verdict_only(struct ai_worker *w, bool verdict_only)
{
	if (w)
		w->verdict_only = verdict_only;
}

int ai_worker_result_tile(struct ai_worker *w, uint64_t ts,
			  uint16_t tile_idx, struct ai_box *boxes,
			  int *box_count, float *max_score)
{
	if (box_count)
		*box_count = 0;
	if (max_score)
		*max_score = 0.0f;
	if (!w)
		return -1;
	std::lock_guard<std::mutex> lock(w->m);
	auto it = w->results.find({ts, tile_idx});
	if (it == w->results.end())
		return -1;
	if (max_score)
		*max_score = it->second.max_score;
	if (it->second.verdict == 1 && boxes && box_count) {
		int n = (int)it->second.boxes.size();
		if (n > AI_MAX_BOXES)
			n = AI_MAX_BOXES;
		memcpy(boxes, it->second.boxes.data(),
		       (size_t)n * sizeof(ai_box));
		*box_count = n;
	}
	return it->second.verdict;
}

void ai_worker_stats(struct ai_worker *w, uint64_t *processed,
		     uint64_t *dropped, uint64_t *nsfw, double *avg_ms)
{
	if (!w)
		return;
	std::lock_guard<std::mutex> lock(w->m);
	if (processed)
		*processed = w->processed;
	if (dropped)
		*dropped = w->dropped;
	if (nsfw)
		*nsfw = w->nsfw;
	if (avg_ms)
		*avg_ms = w->processed ? w->total_ms / (double)w->processed
				       : 0.0;
}

bool ai_worker_using_gpu(struct ai_worker *w)
{
	return w && w->gpu;
}

} // extern "C"
