#pragma once
#include <string>

// Path rules for the network API. Everything here is plain Win32, with no dependency on the game.
namespace SafePath {
	// Checks one path component (a folder or file name). Rejects empty names, ".", "..", reserved Windows device
	// names (CON, NUL, COM1...), invalid characters (including ':' so alternate data streams can't be used),
	// control characters and names ending in a dot or a space.
	bool IsValidComponent(const std::wstring& component, std::string& error);

	// Resolves a UTF-8 path from Lua that must lie strictly inside `root` (a trusted, absolute path).
	// The path must start with `root` (case-insensitive) and the part below it is checked component by component,
	// so "..", relative, UNC, "\\?\" and external absolute paths are all rejected. `exactDepth` is the number of
	// components required below `root` (0 = one or more). `resolved` is built from the trusted `root` spelling.
	bool ResolveInside(const std::wstring& root, const std::string& utf8Path, int exactDepth, std::wstring& resolved, std::string& error);

	// Fails if an existing component of `path` below `root` is a symbolic link or a junction.
	// `root` itself and its parents are not checked (Documents can legitimately be redirected, e.g. OneDrive).
	bool CheckNoLinks(const std::wstring& root, const std::wstring& path, std::string& error);

	// Creates every missing folder of `dir` below `root`. `dir` must already be validated and inside `root`.
	bool EnsureDirectories(const std::wstring& root, const std::wstring& dir, std::string& error);

	// Deletes a folder tree. Links are removed, never followed. Best effort; returns false if something remains.
	bool RemoveTree(const std::wstring& dir);

	// Longest path (in UTF-16 units) the network API will create. Keeps clear of MAX_PATH.
	constexpr size_t MaxPathLength = 240;
}
