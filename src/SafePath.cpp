#include "SafePath.h"
#include <Windows.h>
#include <vector>

namespace SafePath {
	static std::wstring Upper(const std::wstring& s) {
		std::wstring result = s;
		if (!result.empty())
			CharUpperBuffW(result.data(), static_cast<DWORD>(result.size()));
		return result;
	}

	static bool IsReservedName(const std::wstring& component) {
		// Windows treats "NUL.txt" and "NUL " like "NUL".
		std::wstring base = component.substr(0, component.find(L'.'));
		while (!base.empty() && base.back() == L' ')
			base.pop_back();
		base = Upper(base);
		if (base == L"CON" || base == L"PRN" || base == L"AUX" || base == L"NUL" ||
			base == L"CONIN$" || base == L"CONOUT$")
			return true;
		if (base.size() == 4 && (base.compare(0, 3, L"COM") == 0 || base.compare(0, 3, L"LPT") == 0)) {
			wchar_t d = base[3];
			if ((d >= L'0' && d <= L'9') || d == 0x00B9 || d == 0x00B2 || d == 0x00B3)
				return true;
		}
		return false;
	}

	bool IsValidComponent(const std::wstring& component, std::string& error) {
		if (component.empty()) {
			error = "Path has an empty component.";
			return false;
		}
		if (component == L"." || component == L"..") {
			error = "Path contains '.' or '..'.";
			return false;
		}
		for (wchar_t c : component) {
			if (c < 0x20 || c == L'<' || c == L'>' || c == L':' || c == L'"' || c == L'|' || c == L'?' || c == L'*' ||
				c == L'/' || c == L'\\' || c == 0x7F) {
				error = "Path contains an invalid character.";
				return false;
			}
		}
		if (component.back() == L'.' || component.back() == L' ') {
			error = "Path component ends with a dot or a space.";
			return false;
		}
		if (IsReservedName(component)) {
			error = "Path contains a reserved Windows name.";
			return false;
		}
		return true;
	}

	static std::wstring NormalizeSeparators(std::wstring s) {
		for (wchar_t& c : s)
			if (c == L'/') c = L'\\';
		while (s.size() > 1 && s.back() == L'\\')
			s.pop_back();
		return s;
	}

	bool ResolveInside(const std::wstring& root, const std::string& utf8Path, int exactDepth, std::wstring& resolved, std::string& error) {
		if (utf8Path.empty()) {
			error = "Path is empty.";
			return false;
		}
		int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Path.c_str(), static_cast<int>(utf8Path.size()), nullptr, 0);
		if (count <= 0) {
			error = "Path is not valid UTF-8.";
			return false;
		}
		std::wstring path(count, L'\0');
		MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Path.c_str(), static_cast<int>(utf8Path.size()), path.data(), count);
		path = NormalizeSeparators(path);

		std::wstring base = NormalizeSeparators(root);
		if (base.empty()) {
			error = "The base folder is not known.";
			return false;
		}
		if (path.size() <= base.size() + 1 || path[base.size()] != L'\\' ||
			CompareStringOrdinal(path.c_str(), static_cast<int>(base.size()), base.c_str(), static_cast<int>(base.size()), TRUE) != CSTR_EQUAL) {
			error = "Path is outside the allowed folder.";
			return false;
		}

		std::wstring relative = path.substr(base.size() + 1);
		std::wstring result = base;
		int depth = 0;
		size_t start = 0;
		while (true) {
			size_t end = relative.find(L'\\', start);
			std::wstring component = relative.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
			if (!IsValidComponent(component, error))
				return false;
			result += L'\\';
			result += component;
			depth++;
			if (end == std::wstring::npos)
				break;
			start = end + 1;
		}
		if (exactDepth > 0 && depth != exactDepth) {
			error = exactDepth == 1 ? "Path must be directly inside the allowed folder." : "Path has the wrong depth.";
			return false;
		}
		if (result.size() > MaxPathLength) {
			error = "Path is too long.";
			return false;
		}
		resolved = result;
		return true;
	}

	static bool IsLink(const std::wstring& path) {
		WIN32_FIND_DATAW data;
		HANDLE h = FindFirstFileW(path.c_str(), &data);
		if (h == INVALID_HANDLE_VALUE)
			return false;
		FindClose(h);
		if (!(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
			return false;
		// Cloud placeholders (OneDrive) are reparse points too, but they are not links.
		return data.dwReserved0 == IO_REPARSE_TAG_SYMLINK || data.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT;
	}

	bool CheckNoLinks(const std::wstring& root, const std::wstring& path, std::string& error) {
		std::wstring base = NormalizeSeparators(root);
		size_t pos = base.size();
		while (pos < path.size()) {
			size_t next = path.find(L'\\', pos + 1);
			std::wstring prefix = path.substr(0, next == std::wstring::npos ? std::wstring::npos : next);
			DWORD attrs = GetFileAttributesW(prefix.c_str());
			if (attrs == INVALID_FILE_ATTRIBUTES)
				return true; // Nothing deeper exists yet.
			if (IsLink(prefix)) {
				error = "Path goes through a symbolic link or junction.";
				return false;
			}
			if (next == std::wstring::npos)
				break;
			pos = next;
		}
		return true;
	}

	bool EnsureDirectories(const std::wstring& root, const std::wstring& dir, std::string& error) {
		size_t pos = NormalizeSeparators(root).size();
		while (pos < dir.size()) {
			size_t next = dir.find(L'\\', pos + 1);
			std::wstring prefix = dir.substr(0, next == std::wstring::npos ? std::wstring::npos : next);
			if (!CreateDirectoryW(prefix.c_str(), NULL)) {
				DWORD code = GetLastError();
				DWORD attrs = GetFileAttributesW(prefix.c_str());
				if (code != ERROR_ALREADY_EXISTS || attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
					error = "Could not create a folder (error " + std::to_string(code) + ").";
					return false;
				}
			}
			if (next == std::wstring::npos)
				break;
			pos = next;
		}
		return true;
	}

	bool RemoveTree(const std::wstring& dir) {
		WIN32_FIND_DATAW data;
		HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &data);
		if (h != INVALID_HANDLE_VALUE) {
			do {
				std::wstring name = data.cFileName;
				if (name == L"." || name == L"..")
					continue;
				std::wstring full = dir + L"\\" + name;
				bool isDir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
				bool isReparse = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
				if (isDir && !isReparse) {
					RemoveTree(full);
				}
				else {
					SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
					if (isDir)
						RemoveDirectoryW(full.c_str());
					else
						DeleteFileW(full.c_str());
				}
			} while (FindNextFileW(h, &data));
			FindClose(h);
		}
		return RemoveDirectoryW(dir.c_str()) != 0;
	}
}
