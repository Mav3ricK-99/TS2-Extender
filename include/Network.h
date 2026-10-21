#pragma once
#include "ts2/lua.h"
#include <cstdint>
#include <string>
#include <vector>

// Glue between the Lua API and the HTTP / zip code. Jobs run on one worker thread (started on first use);
// results wait in a queue until the game thread collects them. The worker never touches Lua.
namespace Network {
	struct Completed {
		lua_State* luaState = nullptr;      // State to call the callback on.
		lua_State* ownerState = nullptr;    // Core's Lua state when the job was queued; if it changed, drop the result.
		int callbackRef = LUA_NOREF;
		bool ok = false;
		std::string error;
		uint64_t bytes = 0;
		// HttpRequest and UploadNeighborhood: the callback gets (ok, err, status, response) instead of (ok, err, bytes).
		bool isRequest = false;
		int status = 0;             // HTTP status, 0 if the server never answered.
		std::string response;       // Response body, also kept when the status was an error.
	};

	// These functions validate their arguments and paths right away. On false, `error` explains why, nothing was queued
	// and the caller keeps ownership of `callbackRef`. On true the job owns it and it comes back through TakeCompleted.

	// GET `url` into `destPath`, which must be inside <user folder>\TS2ExtenderStaging.
	bool QueueDownload(lua_State* luaState, int callbackRef, const std::string& url, const std::string& destPath,
		const std::string& expectedSha256, std::string& error);

	// Extract the zip at `zipPath` (inside the staging folder) into the NEW folder `extractDir`, which must be
	// directly inside <user folder>\Neighborhoods and must not exist.
	bool QueueExtract(lua_State* luaState, int callbackRef, const std::string& zipPath, const std::string& extractDir,
		std::string& error);

	// Zips the neighborhood folder `name` (a folder name such as "N001", inside <user folder>\Neighborhoods) into
	// `<zipDir>\<name>.zip`. `zipDir` must be inside the staging folder; it is created if needed. The neighborhood is first
	// copied to `<zipDir>\Temp` (the game keeps the live one in use) and that copy is deleted when done.
	bool QueueZip(lua_State* luaState, int callbackRef, const std::string& name, const std::string& zipDir,
		std::string& error);

	// POSTs the .zip at `zipPath` (inside the staging folder) as the body of a request to `url` (a full http(s) URL, or a
	// path that starts with "/" which goes to ServerUrl), with Content-Type application/zip. Runs in the lane of downloads,
	// extractions and zips, so a ZipNeighborhood queued before it finishes first. The result is delivered like HttpRequest's.
	bool QueueUpload(lua_State* luaState, int callbackRef, const std::string& url, const std::string& zipPath,
		std::string& error);

	// POSTs the .zip at `zipPath` (inside the staging folder) as the body of a request to `url` (a full http(s) URL, or a
	// path that starts with "/" which goes to ServerUrl), with Content-Type application/zip. Runs in the lane of downloads,
	// extractions and zips, so a ZipNeighborhood queued before it finishes first. The result is delivered like HttpRequest's.
	bool QueueUpload(lua_State* luaState, int callbackRef, const std::string& url, const std::string& zipPath,
		std::string& error);

	// Sends an HTTP request. `url` is a full http(s) URL, or a path that starts with "/" which goes to ServerUrl.
	// `body` and `headers` may be empty. Runs in its own thread, apart from downloads and extractions, so a long
	// download does not hold it back. Same contract as the functions above: false = nothing queued, caller keeps the ref.
	bool QueueRequest(lua_State* luaState, int callbackRef, const std::string& method, const std::string& url,
		const std::string& body, const std::string& headers, std::string& error);

	// Moves the finished jobs to `out`. Call it from the game thread only.
	void TakeCompleted(std::vector<Completed>& out);
}
