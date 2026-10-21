#pragma once
#include <atomic>
#include <cstdint>
#include <string>

// Safe extraction of a .zip into a NEW folder, using miniz. Plain Win32, no dependency on the game or Lua.
namespace ZipExtract {
	struct Limits {
		uint32_t maxEntries = 50000;
		uint64_t maxTotalBytes = 4096ull * 1024 * 1024;  // Total uncompressed size (zip bomb protection).
	};

	struct Result {
		uint32_t files = 0;
		uint64_t bytes = 0;     // Uncompressed bytes written.
		std::string error;      // Set when the call returns false.
	};

	// Extracts `zipPath` into `targetDir`, which must NOT exist yet (its parent must). Nothing is ever overwritten.
	//  1. The whole archive is validated first (names, links, duplicates, limits); on any problem nothing is written.
	//  2. It is extracted into a hidden temporary folder inside `tempRoot`, which must be on the same volume as `targetDir`.
	//  3. The temporary folder is renamed to `targetDir` in one step, so the game never sees a half-extracted folder.
	// The contents of the archive become the contents of `targetDir` (no root folder is stripped or expected).
	// On failure the temporary folder is deleted. `cancel` is polled between files.
	bool ExtractToNewDirectory(const std::wstring& zipPath, const std::wstring& tempRoot, const std::wstring& targetDir,
		const Limits& limits, const std::atomic<bool>& cancel, Result& result);
}
