#pragma once
#include <atomic>
#include <cstdint>
#include <string>

// Builds a .zip from a neighborhood folder, using miniz. Plain Win32, no dependency on the game or Lua.
namespace ZipCreate {
	struct Limits {
		uint32_t maxEntries = 50000;
		uint64_t maxTotalBytes = 4096ull * 1024 * 1024;  // Total size of the files to pack (before compressing).
	};

	struct Result {
		uint32_t files = 0;
		uint64_t bytes = 0;     // Size of the .zip written.
		std::string error;      // Set when the call returns false.
	};

	// Packs the folder `sourceDir` (the live neighborhood, which is only read) into `zipPath`.
	//  1. `sourceDir` is copied into `tempDir`, which must NOT exist yet (its parent must). The game keeps the live
	//     neighborhood open, so the zip is never built from it directly.
	//  2. The copy is compressed into `<zipPath>.part`, which replaces `zipPath` only when the archive is complete.
	//  3. `tempDir` is always deleted afterwards, on success and on failure.
	// The contents of `sourceDir` become the root of the archive (no enclosing folder), the same layout
	// ZipExtract::ExtractToNewDirectory expects. Links inside `sourceDir` make the call fail. The parent folder of
	// `zipPath` must exist; an existing `zipPath` is replaced. `cancel` is polled between files.
	bool CreateFromDirectory(const std::wstring& sourceDir, const std::wstring& tempDir, const std::wstring& zipPath,
		const Limits& limits, const std::atomic<bool>& cancel, Result& result);
}
