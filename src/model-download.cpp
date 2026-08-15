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

#include "model-download.h"

#include <obs-module.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include <windows.h>
#include <shlobj.h>
#include <winhttp.h>
#include <bcrypt.h>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")

namespace {

struct model_info {
	const char *id;
	const wchar_t *url;
	const wchar_t *filename;
	uint64_t size;
	uint8_t sha256[32];
	int input_size;
};

/* Mirrors of the official NudeNet v3.4 weights (GitHub release assets of
 * notAI-tech/NudeNet are not accessible anonymously, hence HuggingFace
 * mirrors). SHA-256 and sizes are verified on download. */
constexpr model_info MODELS[] = {
	{
		"320n",
		L"https://huggingface.co/deepghs/nudenet_onnx/resolve/main/320n.onnx",
		L"320n.onnx",
		12150158ULL,
		{
			0xC1, 0x5D, 0x82, 0x73, 0xAD, 0xAD, 0x2D, 0x0A,
			0x92, 0xF0, 0x14, 0xCC, 0x69, 0xAB, 0x2D, 0x6C,
			0x31, 0x1A, 0x06, 0x77, 0x7A, 0x55, 0x54, 0x5F,
			0x2C, 0x4E, 0xB4, 0x6F, 0x51, 0x91, 0x1F, 0x0F,
		},
		320,
	},
	{
		"640m",
		L"https://huggingface.co/zhangsongbo365/nudenet_onnx/resolve/main/640m.onnx",
		L"640m.onnx",
		103538690ULL,
		{
			0x5F, 0xD4, 0x88, 0xC3, 0x9A, 0xCF, 0xB2, 0x68,
			0xEF, 0xB4, 0xA9, 0x2B, 0xCE, 0x4F, 0xBC, 0x95,
			0x96, 0x7C, 0x05, 0x9B, 0xD7, 0xEE, 0x1C, 0x01,
			0x02, 0x6F, 0xCA, 0xF8, 0x1A, 0xEC, 0x5C, 0x9E,
		},
		640,
	},
};

constexpr size_t NUM_MODELS = sizeof(MODELS) / sizeof(MODELS[0]);

struct model_rt {
	std::atomic<int> state{MODEL_MISSING};
	std::atomic<int> progress{0};
};

model_rt g_rt[NUM_MODELS];
std::atomic<bool> g_cancel{false};
std::thread g_thread;
std::mutex g_thread_mtx;

const model_info *find_model(const char *id)
{
	if (!id)
		return nullptr;
	for (size_t i = 0; i < NUM_MODELS; i++) {
		if (strcmp(MODELS[i].id, id) == 0)
			return &MODELS[i];
	}
	return nullptr;
}

size_t model_index(const model_info *m)
{
	return (size_t)(m - MODELS);
}

std::wstring models_dir()
{
	wchar_t appdata[MAX_PATH];
	if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0,
				    appdata)))
		return L"";
	return std::wstring(appdata) + L"\\fullblur-filter\\models";
}

std::wstring target_path_w(const model_info *m)
{
	std::wstring dir = models_dir();
	if (dir.empty())
		return L"";
	return dir + L"\\" + m->filename;
}

char *w_to_u8(const std::wstring &w)
{
	int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0,
				      nullptr, nullptr);
	if (len <= 0)
		return nullptr;
	char *out = (char *)bmalloc((size_t)len);
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out, len, nullptr,
			    nullptr);
	return out;
}

bool file_exists(const std::wstring &path)
{
	DWORD attr = GetFileAttributesW(path.c_str());
	return attr != INVALID_FILE_ATTRIBUTES &&
	       !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

void ensure_dir(const std::wstring &file_path)
{
	size_t pos = file_path.find_last_of(L'\\');
	if (pos == std::wstring::npos)
		return;
	std::wstring dir = file_path.substr(0, pos);
	for (size_t i = 3; i < dir.size(); i++) {
		if (dir[i] == L'\\') {
			wchar_t saved = dir[i];
			dir[i] = 0;
			CreateDirectoryW(dir.c_str(), nullptr);
			dir[i] = saved;
		}
	}
	CreateDirectoryW(dir.c_str(), nullptr);
}

struct sha256_ctx {
	BCRYPT_ALG_HANDLE alg = nullptr;
	BCRYPT_HASH_HANDLE hash = nullptr;
	~sha256_ctx()
	{
		if (hash)
			BCryptDestroyHash(hash);
		if (alg)
			BCryptCloseAlgorithmProvider(alg, 0);
	}
	bool init()
	{
		return BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM,
						   nullptr, 0) == 0 &&
		       BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0,
					0) == 0;
	}
	bool update(const void *data, size_t len)
	{
		return BCryptHashData(hash, (PUCHAR)data, (ULONG)len, 0) == 0;
	}
	bool finish_matches(const uint8_t expected[32])
	{
		uint8_t digest[32];
		if (BCryptFinishHash(hash, digest, sizeof(digest), 0) != 0)
			return false;
		return memcmp(digest, expected, sizeof(digest)) == 0;
	}
};

void download_thread(const model_info *m, size_t idx)
{
	auto fail = [&](const char *what) {
		blog(LOG_ERROR,
		     "[fullblur-filter] Model '%s' download failed: %s",
		     m->id, what);
		g_rt[idx].state = MODEL_FAILED;
	};

	std::wstring target = target_path_w(m);
	if (target.empty()) {
		fail("cannot resolve %APPDATA%");
		return;
	}
	std::wstring part = target + L".part";

	URL_COMPONENTS uc = {};
	uc.dwStructSize = sizeof(uc);
	wchar_t host[256], path[1024];
	uc.lpszHostName = host;
	uc.dwHostNameLength = _countof(host);
	uc.lpszUrlPath = path;
	uc.dwUrlPathLength = _countof(path);
	if (!WinHttpCrackUrl(m->url, 0, 0, &uc)) {
		fail("bad URL");
		return;
	}

	HINTERNET session = WinHttpOpen(L"fullblur-filter/1.0",
					WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
					WINHTTP_NO_PROXY_NAME,
					WINHTTP_NO_PROXY_BYPASS, 0);
	if (!session) {
		fail("WinHttpOpen");
		return;
	}
	WinHttpSetTimeouts(session, 15000, 15000, 30000, 30000);

	HINTERNET conn = WinHttpConnect(session, host, uc.nPort, 0);
	if (!conn) {
		WinHttpCloseHandle(session);
		fail("WinHttpConnect");
		return;
	}

	HINTERNET req = WinHttpOpenRequest(
		conn, L"GET", path, nullptr, WINHTTP_NO_REFERER,
		WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
	if (!req) {
		WinHttpCloseHandle(conn);
		WinHttpCloseHandle(session);
		fail("WinHttpOpenRequest");
		return;
	}

	if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
				WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
	    !WinHttpReceiveResponse(req, nullptr)) {
		WinHttpCloseHandle(req);
		WinHttpCloseHandle(conn);
		WinHttpCloseHandle(session);
		fail("no response");
		return;
	}

	DWORD status = 0, sz = sizeof(status);
	WinHttpQueryHeaders(req,
			    WINHTTP_QUERY_STATUS_CODE |
				    WINHTTP_QUERY_FLAG_NUMBER,
			    WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
			    WINHTTP_NO_HEADER_INDEX);
	if (status != 200) {
		WinHttpCloseHandle(req);
		WinHttpCloseHandle(conn);
		WinHttpCloseHandle(session);
		fail("HTTP status != 200");
		return;
	}

	DWORD content_len = 0;
	sz = sizeof(content_len);
	WinHttpQueryHeaders(req,
			    WINHTTP_QUERY_CONTENT_LENGTH |
				    WINHTTP_QUERY_FLAG_NUMBER,
			    WINHTTP_HEADER_NAME_BY_INDEX, &content_len, &sz,
			    WINHTTP_NO_HEADER_INDEX);

	ensure_dir(target);
	HANDLE file = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr,
				  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
				  nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		WinHttpCloseHandle(req);
		WinHttpCloseHandle(conn);
		WinHttpCloseHandle(session);
		fail("cannot create file");
		return;
	}

	sha256_ctx sha;
	if (!sha.init()) {
		CloseHandle(file);
		WinHttpCloseHandle(req);
		WinHttpCloseHandle(conn);
		WinHttpCloseHandle(session);
		fail("BCrypt init");
		return;
	}

	uint8_t buf[64 * 1024];
	uint64_t total = 0;
	bool ok = true;
	for (;;) {
		if (g_cancel.load()) {
			ok = false;
			break;
		}
		DWORD avail = 0;
		if (!WinHttpQueryDataAvailable(req, &avail)) {
			ok = false;
			break;
		}
		if (avail == 0)
			break;
		DWORD to_read = avail > sizeof(buf) ? (DWORD)sizeof(buf)
						    : avail;
		DWORD got = 0;
		if (!WinHttpReadData(req, buf, to_read, &got)) {
			ok = false;
			break;
		}
		if (got == 0)
			continue;
		DWORD written = 0;
		if (!WriteFile(file, buf, got, &written, nullptr) ||
		    written != got || !sha.update(buf, got)) {
			ok = false;
			break;
		}
		total += got;
		uint64_t denom = content_len ? content_len : m->size;
		int pct = (int)(total * 100 / denom);
		g_rt[idx].progress = pct > 99 ? 99 : pct;
	}

	CloseHandle(file);
	WinHttpCloseHandle(req);
	WinHttpCloseHandle(conn);
	WinHttpCloseHandle(session);

	if (!ok) {
		DeleteFileW(part.c_str());
		if (g_cancel.load())
			return;
		fail("read/write error");
		return;
	}
	if (total != m->size) {
		DeleteFileW(part.c_str());
		fail("size mismatch");
		return;
	}
	if (!sha.finish_matches(m->sha256)) {
		DeleteFileW(part.c_str());
		fail("SHA-256 mismatch");
		return;
	}
	if (!MoveFileExW(part.c_str(), target.c_str(),
			 MOVEFILE_REPLACE_EXISTING)) {
		DeleteFileW(part.c_str());
		fail("rename");
		return;
	}

	g_rt[idx].progress = 100;
	g_rt[idx].state = MODEL_READY;
	blog(LOG_INFO, "[fullblur-filter] Model '%s' downloaded to %ls",
	     m->id, target.c_str());
}

} // namespace

int model_count(void)
{
	return (int)NUM_MODELS;
}

const char *model_id_at(int index)
{
	if (index < 0 || (size_t)index >= NUM_MODELS)
		return nullptr;
	return MODELS[index].id;
}

bool model_id_valid(const char *id)
{
	return find_model(id) != nullptr;
}

int model_input_size(const char *id)
{
	const model_info *m = find_model(id);
	return m ? m->input_size : 320;
}

char *model_find_local(const char *id)
{
	const model_info *m = find_model(id);
	if (!m)
		return nullptr;

	std::wstring target = target_path_w(m);
	if (!target.empty() && file_exists(target))
		return w_to_u8(target);

	std::string rel = std::string("models/") + m->id + ".onnx";
	return obs_module_file(rel.c_str());
}

char *model_target_path(const char *id)
{
	const model_info *m = find_model(id);
	if (!m)
		return nullptr;
	return w_to_u8(target_path_w(m));
}

void model_download_start(const char *id)
{
	const model_info *m = find_model(id);
	if (!m)
		return;
	size_t idx = model_index(m);

	std::lock_guard<std::mutex> lock(g_thread_mtx);
	if (g_rt[idx].state == MODEL_DOWNLOADING ||
	    g_rt[idx].state == MODEL_READY)
		return;
	if (g_thread.joinable()) {
		/* a different model is being downloaded: cancel it */
		g_cancel = true;
		g_thread.join();
	}
	g_cancel = false;
	g_rt[idx].progress = 0;
	g_rt[idx].state = MODEL_DOWNLOADING;
	g_thread = std::thread(download_thread, m, idx);
}

enum model_state model_download_state(const char *id)
{
	const model_info *m = find_model(id);
	if (!m)
		return MODEL_MISSING;
	return (enum model_state)g_rt[model_index(m)].state.load();
}

int model_download_progress(const char *id)
{
	const model_info *m = find_model(id);
	if (!m)
		return 0;
	return g_rt[model_index(m)].progress.load();
}

void model_download_shutdown(void)
{
	std::lock_guard<std::mutex> lock(g_thread_mtx);
	if (g_thread.joinable()) {
		g_cancel = true;
		g_thread.join();
	}
}
