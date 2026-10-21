#include "Network.h"
#include "Core.h"
#include "HttpClient.h"
#include "Logging.h"
#include "SafePath.h"
#include "ZipCreate.h"
#include "ZipExtract.h"
#include "config.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <Windows.h>

namespace Network {
	namespace {
		const size_t MaxPendingJobs = 8;
		const size_t MaxPendingRequests = 32;
		const size_t MaxRequestBodyBytes = 1024 * 1024;
		const uint64_t MaxResponseBytes = 1024 * 1024;
				const wchar_t* StagingFolderName = L"TS2ExtenderStaging";
		const wchar_t* NeighborhoodsFolderName = L"Neighborhoods";

		struct Job {
			enum class Kind { Download, Extract, Request, Zip, Upload };
			Kind kind = Kind::Download;
			lua_State* luaState = nullptr;
			lua_State* ownerState = nullptr;
			int callbackRef = LUA_NOREF;
			// Download
			std::string url;
			std::string expectedSha256;
			std::wstring destPath;
			Http::Options httpOptions;
			// Request (url, headers and httpOptions are shared with Download and Upload)
			std::string method;
			std::string body;
			std::string headers;
			// Extract (Upload also reads its .zip from zipPath)
			std::wstring zipPath;
			std::wstring targetDir;
			ZipExtract::Limits zipLimits;
			// Zip (zipPath is the .zip to create, targetDir the folder that holds it)
			std::wstring sourceDir;
			std::wstring tempDir;
			ZipCreate::Limits zipCreateLimits;
			// Both
			std::wstring userDir;
			std::wstring stagingDir;
		};

		// Lives for the whole process and is never destroyed: the worker thread may still be running when the
		// process shuts down, and it must not find destroyed objects.
		struct State {
			// Downloads and extractions share one worker (they are heavy and run one at a time). HTTP requests get their own, so
			// small and frequent requests are not stuck behind a long download, and still go out in the order requested.
			struct Lane {
				std::condition_variable wake;
				std::deque<Job> jobs;
				size_t running = 0;
				bool workerStarted = false;
			};
			std::mutex mutex;
			Lane lanes[2];                  // 0 = downloads and extractions, 1 = HTTP requests.
			std::vector<Completed> completed;
			std::atomic<bool> cancel{ false };
		};

		int LaneOf(const Job& job) { return job.kind == Job::Kind::Request ? 1 : 0; }

		State& GetState() {
			static State* state = new State();
			return *state;
		}

		uint64_t MegabytesToBytes(int megabytes, int fallback) {
			if (megabytes <= 0) megabytes = fallback;
			return static_cast<uint64_t>(megabytes) * 1024 * 1024;
		}

		Completed RunJob(const Job& job) {
			State& state = GetState();
			Completed done;
			done.luaState = job.luaState;
			done.ownerState = job.ownerState;
			done.callbackRef = job.callbackRef;
			done.isRequest = job.kind == Job::Kind::Request || job.kind == Job::Kind::Upload;
			std::string linkError;
			try {
				if (job.kind == Job::Kind::Request) {
					Http::Result result;
					done.ok = Http::Request(job.method, job.url, job.body, job.headers, job.httpOptions, state.cancel, result);
					done.status = result.status;
					done.response = std::move(result.body);
					done.error = result.error;
				}
				else if (job.kind == Job::Kind::Upload) {
					if (!SafePath::CheckNoLinks(job.userDir, job.zipPath, linkError)) {
						done.error = linkError;
						return done;
					}
					Http::Result result;
					done.ok = Http::PostFile(job.url, job.zipPath, job.headers, job.httpOptions, state.cancel, result);
					done.status = result.status;
					done.response = std::move(result.body);
					done.bytes = result.bytes;
					done.error = result.error;
				}
				else if (job.kind == Job::Kind::Download) {
					if (!SafePath::CheckNoLinks(job.userDir, job.destPath, linkError) ||
						!SafePath::EnsureDirectories(job.stagingDir, job.destPath.substr(0, job.destPath.find_last_of(L'\\')), linkError)) {
						done.error = linkError;
						return done;
					}
					Http::Result result;
					done.ok = Http::GetToFile(job.url, job.destPath, job.expectedSha256, job.httpOptions, state.cancel, result);
					done.bytes = result.bytes;
					done.error = result.error;
				}
				else if (job.kind == Job::Kind::Zip) {
					if (!SafePath::CheckNoLinks(job.userDir, job.sourceDir, linkError) ||
						!SafePath::CheckNoLinks(job.userDir, job.tempDir, linkError) ||
						!SafePath::CheckNoLinks(job.userDir, job.zipPath, linkError)) {
						done.error = linkError;
						return done;
					}
					// Jobs of this lane run one at a time, so a Temp folder that is still here is a leftover of an interrupted run.
					if (GetFileAttributesW(job.tempDir.c_str()) != INVALID_FILE_ATTRIBUTES)
						SafePath::RemoveTree(job.tempDir);
					ZipCreate::Result result;
					done.ok = ZipCreate::CreateFromDirectory(job.sourceDir, job.tempDir, job.zipPath, job.zipCreateLimits, state.cancel, result);
					done.bytes = result.bytes;
					done.error = result.error;
				}
				else {
					if (!SafePath::CheckNoLinks(job.userDir, job.targetDir, linkError) ||
						!SafePath::CheckNoLinks(job.userDir, job.zipPath, linkError)) {
						done.error = linkError;
						return done;
					}
					ZipExtract::Result result;
					done.ok = ZipExtract::ExtractToNewDirectory(job.zipPath, job.stagingDir, job.targetDir, job.zipLimits, state.cancel, result);
					done.bytes = result.bytes;
					done.error = result.error;
				}
			}
			catch (const std::exception& e) {
				done.ok = false;
				done.error = std::string("Unexpected error: ") + e.what();
			}
			catch (...) {
				done.ok = false;
				done.error = "Unexpected error.";
			}
			if (!done.ok) done.bytes = 0;
			return done;
		}

		void WorkerMain(int laneIndex) {
			State& state = GetState();
			State::Lane& lane = state.lanes[laneIndex];
			while (true) {
				Job job;
				{
					std::unique_lock<std::mutex> lock(state.mutex);
					lane.wake.wait(lock, [&] { return !lane.jobs.empty(); });
					job = std::move(lane.jobs.front());
					lane.jobs.pop_front();
					lane.running = 1;
				}
				Completed done = RunJob(job);
				{
					std::lock_guard<std::mutex> lock(state.mutex);
					state.completed.push_back(std::move(done));
					lane.running = 0;
				}
			}
		}

		bool Enqueue(Job&& job, std::string& error) {
			State& state = GetState();
			std::lock_guard<std::mutex> lock(state.mutex);
			int laneIndex = LaneOf(job);
			State::Lane& lane = state.lanes[laneIndex];
			if (lane.jobs.size() + lane.running >= (laneIndex == 1 ? MaxPendingRequests : MaxPendingJobs)) {
				error = "Too many network operations in progress.";
				return false;
			}
			if (!lane.workerStarted) {
				// Started on first use and not in DllMain, where creating threads can deadlock the loader lock.
				try {
					std::thread(WorkerMain, laneIndex).detach();
				}
				catch (const std::exception&) {
					error = "Could not start the network thread.";
					return false;
				}
				lane.workerStarted = true;
			}
			lane.jobs.push_back(std::move(job));
			lane.wake.notify_one();
			return true;
		}

		// A full URL is used as it is; a path that starts with "/" goes to the ServerUrl of TS2Extender.ini.
		bool ResolveUrl(const std::string& url, std::string& resolved, std::string& error) {
			if (url.empty()) { error = "url is empty."; return false; }
			if (url[0] != '/') {
				resolved = url;
				return true;
			}
			std::string server = Config::ServerUrl;
			while (!server.empty() && server.back() == '/') server.pop_back();
			if (server.empty()) {
				error = "url is a path, but ServerUrl is not set in the [Network] section of TS2Extender.ini.";
				return false;
			}
			resolved = server + url;
			return true;
		}

		// Common part of all jobs: where the user folder and the staging folder are.
		bool PrepareFolders(Job& job, std::string& error) {
			job.userDir = Core::_instance->m_UserDataPath;
			if (job.userDir.empty()) {
				error = "The game's user folder is not known.";
				return false;
			}
			while (job.userDir.size() > 1 && (job.userDir.back() == L'\\' || job.userDir.back() == L'/'))
				job.userDir.pop_back();
			job.stagingDir = job.userDir + L"\\" + StagingFolderName;
			return true;
		}

		bool EnsureStagingFolder(const Job& job, std::string& error) {
			return SafePath::CheckNoLinks(job.userDir, job.stagingDir, error) &&
				SafePath::EnsureDirectories(job.userDir, job.stagingDir, error);
		}
	}

	bool QueueDownload(lua_State* luaState, int callbackRef, const std::string& url, const std::string& destPath,
		const std::string& expectedSha256, std::string& error) {
		if (url.empty()) { error = "url is empty."; return false; }
		if (!expectedSha256.empty() && !Http::IsSha256Hex(expectedSha256)) {
			error = "expectedSha256 must be 64 hexadecimal characters.";
			return false;
		}

		Job job;
		job.kind = Job::Kind::Download;
		job.luaState = luaState;
		job.ownerState = Core::_instance->m_LuaState;
		job.callbackRef = callbackRef;
		if (!ResolveUrl(url, job.url, error)) return false;
		job.expectedSha256 = expectedSha256;
		if (!PrepareFolders(job, error)) return false;

		if (!SafePath::ResolveInside(job.stagingDir, destPath, 0, job.destPath, error)) {
			error = "destPath must be inside the staging folder (" + std::string("<user folder>\\") +
				"TS2ExtenderStaging): " + error;
			return false;
		}
		if (!SafePath::CheckNoLinks(job.userDir, job.destPath, error)) return false;
		DWORD attrs = GetFileAttributesW(job.destPath.c_str());
		if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
			error = "destPath is an existing folder.";
			return false;
		}
		if (!EnsureStagingFolder(job, error)) return false;

		job.httpOptions.maxBytes = MegabytesToBytes(Config::DownloadMaxMegabytes, 1024);
		job.httpOptions.totalTimeoutMs = static_cast<DWORD>(Config::DownloadTimeoutSeconds > 0 ? Config::DownloadTimeoutSeconds : 600) * 1000;
		return Enqueue(std::move(job), error);
	}

	bool QueueExtract(lua_State* luaState, int callbackRef, const std::string& zipPath, const std::string& extractDir,
		std::string& error) {
		Job job;
		job.kind = Job::Kind::Extract;
		job.luaState = luaState;
		job.ownerState = Core::_instance->m_LuaState;
		job.callbackRef = callbackRef;
		if (!PrepareFolders(job, error)) return false;

		if (!SafePath::ResolveInside(job.stagingDir, zipPath, 0, job.zipPath, error)) {
			error = "zipPath must be inside the staging folder (<user folder>\\TS2ExtenderStaging): " + error;
			return false;
		}
		std::wstring hoodsDir = job.userDir + L"\\" + NeighborhoodsFolderName;
		if (!SafePath::ResolveInside(hoodsDir, extractDir, 1, job.targetDir, error)) {
			error = "extractDir must be a new folder directly inside the Neighborhoods folder: " + error;
			return false;
		}
		if (!SafePath::CheckNoLinks(job.userDir, job.zipPath, error) || !SafePath::CheckNoLinks(job.userDir, job.targetDir, error))
			return false;

		DWORD zipAttrs = GetFileAttributesW(job.zipPath.c_str());
		if (zipAttrs == INVALID_FILE_ATTRIBUTES || (zipAttrs & FILE_ATTRIBUTE_DIRECTORY)) {
			error = "The zip file was not found.";
			return false;
		}
		DWORD hoodsAttrs = GetFileAttributesW(hoodsDir.c_str());
		if (hoodsAttrs == INVALID_FILE_ATTRIBUTES || !(hoodsAttrs & FILE_ATTRIBUTE_DIRECTORY)) {
			error = "The Neighborhoods folder was not found.";
			return false;
		}
		if (GetFileAttributesW(job.targetDir.c_str()) != INVALID_FILE_ATTRIBUTES) {
			error = "extractDir already exists. Nothing was changed.";
			return false;
		}

		job.zipLimits.maxTotalBytes = MegabytesToBytes(Config::ExtractMaxMegabytes, 4096);
		return Enqueue(std::move(job), error);
	}

	bool QueueZip(lua_State* luaState, int callbackRef, const std::string& name, const std::string& zipDir,
		std::string& error) {
		Job job;
		job.kind = Job::Kind::Zip;
		job.luaState = luaState;
		job.ownerState = Core::_instance->m_LuaState;
		job.callbackRef = callbackRef;
		if (!PrepareFolders(job, error)) return false;

		// `name` is a folder name, never a path: one valid component, which also rules out separators and "..".
		int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.c_str(), static_cast<int>(name.size()), nullptr, 0);
		if (name.empty() || count <= 0) { error = "name must be a non-empty UTF-8 neighborhood folder name, such as N001."; return false; }
		std::wstring wideName(count, L'\0');
		MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.c_str(), static_cast<int>(name.size()), wideName.data(), count);
		std::string nameError;
		if (!SafePath::IsValidComponent(wideName, nameError)) {
			error = "name must be a neighborhood folder name, such as N001: " + nameError;
			return false;
		}

		job.sourceDir = job.userDir + L"\\" + NeighborhoodsFolderName + L"\\" + wideName;
		if (!SafePath::CheckNoLinks(job.userDir, job.sourceDir, error)) return false;
		DWORD sourceAttrs = GetFileAttributesW(job.sourceDir.c_str());
		if (sourceAttrs == INVALID_FILE_ATTRIBUTES || !(sourceAttrs & FILE_ATTRIBUTE_DIRECTORY)) {
			error = "The neighborhood folder was not found in Neighborhoods.";
			return false;
		}

		if (!SafePath::ResolveInside(job.stagingDir, zipDir, 0, job.targetDir, error)) {
			error = "zipDir must be a folder inside the staging folder (<user folder>\\TS2ExtenderStaging): " + error;
			return false;
		}
		job.tempDir = job.targetDir + L"\\Temp";
		job.zipPath = job.targetDir + L"\\" + wideName + L".zip";
		// The .part file written next to the zip must fit as well.
		if (job.zipPath.size() + 5 > SafePath::MaxPathLength) { error = "zipDir plus the neighborhood name make a path that is too long."; return false; }
		if (!SafePath::CheckNoLinks(job.userDir, job.targetDir, error)) return false;
		DWORD dirAttrs = GetFileAttributesW(job.targetDir.c_str());
		if (dirAttrs != INVALID_FILE_ATTRIBUTES && !(dirAttrs & FILE_ATTRIBUTE_DIRECTORY)) {
			error = "zipDir is an existing file.";
			return false;
		}
		if (!EnsureStagingFolder(job, error) || !SafePath::EnsureDirectories(job.stagingDir, job.targetDir, error)) return false;
		DWORD zipAttrs = GetFileAttributesW(job.zipPath.c_str());
		if (zipAttrs != INVALID_FILE_ATTRIBUTES && (zipAttrs & FILE_ATTRIBUTE_DIRECTORY)) {
			error = "The zip path is an existing folder.";
			return false;
		}

		job.zipCreateLimits.maxTotalBytes = MegabytesToBytes(Config::ExtractMaxMegabytes, 4096);
		return Enqueue(std::move(job), error);
	}

	bool QueueUpload(lua_State* luaState, int callbackRef, const std::string& url, const std::string& zipPath,
		std::string& error) {
		Job job;
		job.kind = Job::Kind::Upload;
		job.luaState = luaState;
		job.ownerState = Core::_instance->m_LuaState;
		job.callbackRef = callbackRef;
		if (!ResolveUrl(url, job.url, error)) return false;
		if (!PrepareFolders(job, error)) return false;

		if (!SafePath::ResolveInside(job.stagingDir, zipPath, 0, job.zipPath, error)) {
			error = "zipPath must be inside the staging folder (<user folder>\\TS2ExtenderStaging): " + error;
			return false;
		}
		if (!SafePath::CheckNoLinks(job.userDir, job.zipPath, error)) return false;
		// Only .zip files leave the machine through this function: it is not a general file uploader.
		if (job.zipPath.size() < 5 || _wcsicmp(job.zipPath.c_str() + job.zipPath.size() - 4, L".zip") != 0) {
			error = "zipPath must be a .zip file.";
			return false;
		}
		WIN32_FILE_ATTRIBUTE_DATA info;
		if (!GetFileAttributesExW(job.zipPath.c_str(), GetFileExInfoStandard, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
			error = "The zip file was not found.";
			return false;
		}
		uint64_t size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
		if (size == 0) { error = "The zip file is empty."; return false; }
		uint64_t maxBytes = (std::min<uint64_t>)(MegabytesToBytes(Config::UploadMaxMegabytes, 1024), 0xFFFFFFFFull);
		if (size > maxBytes) { error = "The zip file is bigger than the allowed upload size."; return false; }

		job.headers = "Content-Type: application/zip\r\n";
		job.httpOptions.maxBytes = maxBytes;
		job.httpOptions.maxRedirects = 0;      // A redirected POST would be sent again as a GET without its body.
		job.httpOptions.totalTimeoutMs = static_cast<DWORD>(Config::DownloadTimeoutSeconds > 0 ? Config::DownloadTimeoutSeconds : 600) * 1000;
		return Enqueue(std::move(job), error);
	}

	bool QueueRequest(lua_State* luaState, int callbackRef, const std::string& method, const std::string& url,
		const std::string& body, const std::string& headers, std::string& error) {
		Job job;
		job.kind = Job::Kind::Request;
		job.luaState = luaState;
		job.ownerState = Core::_instance->m_LuaState;
		job.callbackRef = callbackRef;

		job.method = method;
		if (!Http::IsAllowedMethod(job.method)) {
			error = "method must be GET, HEAD, POST, PUT, PATCH, DELETE or OPTIONS (upper case).";
			return false;
		}
		bool hasBody = !body.empty();
		if (hasBody && (job.method == "GET" || job.method == "HEAD")) {
			error = "GET and HEAD requests cannot have a body.";
			return false;
		}
		if (body.size() > MaxRequestBodyBytes) { error = "body is bigger than 1 MB."; return false; }
		job.body = body;
		if (!Http::NormalizeHeaders(headers, job.headers, error)) return false;

		if (!ResolveUrl(url, job.url, error)) return false;

		int timeoutSeconds = Config::HttpTimeoutSeconds > 0 ? Config::HttpTimeoutSeconds : 15;
		job.httpOptions.maxBytes = MaxResponseBytes;
		// Redirects only for GET and HEAD: a redirected POST/PUT/... would be sent again as a GET without its body.
		job.httpOptions.maxRedirects = (job.method == "GET" || job.method == "HEAD") ? 5 : 0;
		job.httpOptions.totalTimeoutMs = static_cast<DWORD>(timeoutSeconds) * 1000;
		job.httpOptions.connectTimeoutMs = (std::min)(job.httpOptions.connectTimeoutMs, job.httpOptions.totalTimeoutMs);
		job.httpOptions.ioTimeoutMs = (std::min)(job.httpOptions.ioTimeoutMs, job.httpOptions.totalTimeoutMs);
		return Enqueue(std::move(job), error);
	}

	void TakeCompleted(std::vector<Completed>& out) {
		State& state = GetState();
		std::lock_guard<std::mutex> lock(state.mutex);
		for (Completed& c : state.completed)
			out.push_back(std::move(c));
		state.completed.clear();
	}
}
