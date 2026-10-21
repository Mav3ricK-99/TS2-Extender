#include "ZipExtract.h"
#include "SafePath.h"
#include "miniz.h"
#include <Windows.h>
#include <algorithm>
#include <set>
#include <vector>

namespace ZipExtract {
	namespace {
		struct Entry {
			mz_uint index;
			bool isDirectory;
			std::wstring path;      // Relative, with backslashes, no trailing separator.
			uint64_t size;          // Declared uncompressed size.
		};

		struct HandleCloser {
			HANDLE h = INVALID_HANDLE_VALUE;
			~HandleCloser() { Close(); }
			void Close() { if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); h = INVALID_HANDLE_VALUE; } }
		};

		size_t ReadFromFile(void* opaque, mz_uint64 offset, void* buffer, size_t size) {
			HANDLE h = static_cast<HANDLE>(opaque);
			LARGE_INTEGER position;
			position.QuadPart = static_cast<LONGLONG>(offset);
			if (!SetFilePointerEx(h, position, NULL, FILE_BEGIN))
				return 0;
			size_t total = 0;
			while (total < size) {
				DWORD chunk = static_cast<DWORD>(std::min<size_t>(size - total, 1u << 20));
				DWORD got = 0;
				if (!ReadFile(h, static_cast<char*>(buffer) + total, chunk, &got, NULL) || got == 0)
					break;
				total += got;
			}
			return total;
		}

		// State of the current file while miniz hands us decompressed data.
		struct WriteState {
			HANDLE file;
			uint64_t written;
			uint64_t declared;
			uint64_t* totalBytes;
			uint64_t maxTotal;
			const std::atomic<bool>* cancel;
			bool failed;
		};

		size_t WriteToFile(void* opaque, mz_uint64, const void* buffer, size_t size) {
			WriteState* state = static_cast<WriteState*>(opaque);
			if (state->failed || state->cancel->load())
				return 0;
			// Bounded by what the archive declared and by the global limit, even if the archive lies.
			if (state->written + size > state->declared || *state->totalBytes + size > state->maxTotal) {
				state->failed = true;
				return 0;
			}
			size_t done = 0;
			while (done < size) {
				DWORD chunk = static_cast<DWORD>(std::min<size_t>(size - done, 1u << 20));
				DWORD wrote = 0;
				if (!WriteFile(state->file, static_cast<const char*>(buffer) + done, chunk, &wrote, NULL) || wrote != chunk) {
					state->failed = true;
					return 0;
				}
				done += wrote;
			}
			state->written += size;
			*state->totalBytes += size;
			return size;
		}

		bool Widen(const char* utf8, std::wstring& out) {
			int len = static_cast<int>(strlen(utf8));
			int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, len, nullptr, 0);
			if (count <= 0) return false;
			out.assign(count, L'\0');
			MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, len, out.data(), count);
			return true;
		}

		std::wstring Lower(const std::wstring& s) {
			std::wstring r = s;
			if (!r.empty()) CharLowerBuffW(r.data(), static_cast<DWORD>(r.size()));
			return r;
		}

		bool Exists(const std::wstring& path) {
			return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
		}

		std::string Code(DWORD code) {
			return " (error " + std::to_string(code) + ")";
		}

		std::wstring ParentOf(const std::wstring& path) {
			size_t slash = path.find_last_of(L'\\');
			return slash == std::wstring::npos ? L"" : path.substr(0, slash);
		}

		// Validates every entry of the archive and fills `entries`. Writes nothing.
		bool ValidateArchive(mz_zip_archive& zip, const std::wstring& longestRoot, const Limits& limits,
			std::vector<Entry>& entries, std::string& error) {
			mz_uint count = mz_zip_reader_get_num_files(&zip);
			if (count == 0) { error = "The zip is empty."; return false; }
			if (count > limits.maxEntries) { error = "The zip has too many entries."; return false; }

			std::set<std::wstring> files, dirs;
			uint64_t total = 0;
			entries.reserve(count);

			for (mz_uint i = 0; i < count; i++) {
				mz_zip_archive_file_stat stat;
				if (!mz_zip_reader_file_stat(&zip, i, &stat)) { error = "The zip is damaged."; return false; }

				if (mz_zip_reader_is_file_encrypted(&zip, i)) { error = "The zip has encrypted entries."; return false; }
				if (!mz_zip_reader_is_file_supported(&zip, i)) { error = "The zip uses an unsupported compression method."; return false; }

				bool isDirectory = mz_zip_reader_is_file_a_directory(&zip, i) != 0;

				// Links and special files. Unix-made zips keep the file mode in the high 16 bits of the external attributes.
				if ((stat.m_version_made_by >> 8) == 3) {
					unsigned mode = (stat.m_external_attr >> 16) & 0170000;
					if (mode != 0 && mode != 0100000 && mode != 0040000) { error = "The zip has links or special files."; return false; }
				}
				if ((stat.m_external_attr & 0xFFFF) & FILE_ATTRIBUTE_REPARSE_POINT) { error = "The zip has links or special files."; return false; }

				std::wstring name;
				if (!Widen(stat.m_filename, name)) { error = "The zip has an entry name that is not valid UTF-8."; return false; }
				if (name.find(L'\\') != std::wstring::npos) { error = "The zip has an entry name with a backslash."; return false; }
				if (isDirectory && !name.empty() && name.back() == L'/') name.pop_back();
				if (name.empty() || name[0] == L'/') { error = "The zip has an absolute or empty entry name."; return false; }

				std::wstring path;
				size_t start = 0;
				while (true) {
					size_t end = name.find(L'/', start);
					std::wstring component = name.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
					std::string reason;
					if (!SafePath::IsValidComponent(component, reason)) {
						error = "The zip has an unsafe entry name: " + reason;
						return false;
					}
					if (!path.empty()) path += L'\\';
					path += component;
					if (end == std::wstring::npos) break;
					start = end + 1;
				}
				if (longestRoot.size() + 1 + path.size() > SafePath::MaxPathLength) { error = "The zip has a path that is too long."; return false; }

				std::wstring key = Lower(path);
				if (isDirectory) {
					dirs.insert(key);
				}
				else {
					if (!files.insert(key).second) { error = "The zip has duplicate entries."; return false; }
					if (stat.m_uncomp_size > limits.maxTotalBytes || total + stat.m_uncomp_size > limits.maxTotalBytes) {
						error = "The zip is too big once uncompressed.";
						return false;
					}
					total += stat.m_uncomp_size;
				}
				entries.push_back({ i, isDirectory, path, isDirectory ? 0 : static_cast<uint64_t>(stat.m_uncomp_size) });
			}

			// A name can't be both a file and a folder, nor can a file have a file as parent.
			for (const Entry& e : entries) {
				std::wstring key = Lower(e.path);
				if (e.isDirectory && files.count(key)) { error = "The zip has a name used as both file and folder."; return false; }
				for (size_t slash = key.find(L'\\'); slash != std::wstring::npos; slash = key.find(L'\\', slash + 1)) {
					if (files.count(key.substr(0, slash))) { error = "The zip has a file used as a folder."; return false; }
				}
			}
			return true;
		}

		bool ExtractEntries(mz_zip_archive& zip, const std::vector<Entry>& entries, const std::wstring& tempDir,
			const Limits& limits, const std::atomic<bool>& cancel, Result& result) {
			uint64_t total = 0;
			for (const Entry& e : entries) {
				if (cancel.load()) { result.error = "Cancelled."; return false; }
				std::wstring full = tempDir + L"\\" + e.path;
				std::string dirError;
				if (e.isDirectory) {
					if (!SafePath::EnsureDirectories(tempDir, full, dirError)) { result.error = dirError; return false; }
					continue;
				}
				if (!SafePath::EnsureDirectories(tempDir, ParentOf(full), dirError)) { result.error = dirError; return false; }

				HandleCloser file;
				// CREATE_NEW: this never overwrites anything, even inside the temporary folder.
				file.h = CreateFileW(full.c_str(), GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
				if (file.h == INVALID_HANDLE_VALUE) { result.error = "Could not create a file" + Code(GetLastError()) + "."; return false; }

				WriteState state{ file.h, 0, e.size, &total, limits.maxTotalBytes, &cancel, false };
				mz_bool ok = mz_zip_reader_extract_to_callback(&zip, e.index, WriteToFile, &state, 0);
				if (!ok || state.written != e.size) {
					result.error = cancel.load() ? "Cancelled." :
						state.failed ? "Could not write the extracted data (disk full, or the zip is lying about sizes)." :
						std::string("The zip is damaged: ") + mz_zip_get_error_string(mz_zip_get_last_error(&zip)) + ".";
					return false;
				}
				result.files++;
			}
			result.bytes = total;
			return true;
		}
	}

	static std::wstring MakeTempDirName(const std::wstring& tempRoot) {
		static std::atomic<unsigned> counter{ 0 };
		return tempRoot + L"\\extract_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
			std::to_wstring(GetTickCount64()) + L"_" + std::to_wstring(counter++);
	}

	bool ExtractToNewDirectory(const std::wstring& zipPath, const std::wstring& tempRoot, const std::wstring& targetDir,
		const Limits& limits, const std::atomic<bool>& cancel, Result& result) {
		result = Result{};

		if (Exists(targetDir)) { result.error = "The destination folder already exists. Nothing was changed."; return false; }
		std::wstring parent = ParentOf(targetDir);
		DWORD parentAttrs = GetFileAttributesW(parent.c_str());
		if (parentAttrs == INVALID_FILE_ATTRIBUTES || !(parentAttrs & FILE_ATTRIBUTE_DIRECTORY)) {
			result.error = "The parent folder of the destination does not exist.";
			return false;
		}

		HandleCloser zipFile;
		zipFile.h = CreateFileW(zipPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (zipFile.h == INVALID_HANDLE_VALUE) { result.error = "Could not open the zip file" + Code(GetLastError()) + "."; return false; }
		LARGE_INTEGER zipSize;
		if (!GetFileSizeEx(zipFile.h, &zipSize) || zipSize.QuadPart <= 0) { result.error = "The zip file is empty."; return false; }

		mz_zip_archive zip;
		mz_zip_zero_struct(&zip);
		zip.m_pRead = ReadFromFile;
		zip.m_pIO_opaque = zipFile.h;
		if (!mz_zip_reader_init(&zip, static_cast<mz_uint64>(zipSize.QuadPart), 0)) {
			result.error = std::string("The file is not a valid zip: ") + mz_zip_get_error_string(mz_zip_get_last_error(&zip)) + ".";
			return false;
		}

		std::wstring tempDir = MakeTempDirName(tempRoot);
		bool ok = false;
		bool tempCreated = false;
		try {
			std::vector<Entry> entries;
			const std::wstring& longest = tempDir.size() > targetDir.size() ? tempDir : targetDir;
			if (ValidateArchive(zip, longest, limits, entries, result.error)) {
				if (!CreateDirectoryW(tempDir.c_str(), NULL)) {
					result.error = "Could not create the temporary folder" + Code(GetLastError()) + ".";
				}
				else {
					tempCreated = true;
					ok = ExtractEntries(zip, entries, tempDir, limits, cancel, result);
				}
			}
		}
		catch (const std::exception& e) {
			result.error = std::string("Unexpected error: ") + e.what();
			ok = false;
		}
		mz_zip_reader_end(&zip);
		zipFile.Close();

		if (ok) {
			// Antivirus or indexers may hold the new folder for a moment; MoveFileEx without REPLACE_EXISTING never overwrites.
			DWORD code = 0;
			ok = false;
			for (int attempt = 0; attempt < 10 && !ok; attempt++) {
				if (attempt > 0) Sleep(200);
				if (MoveFileExW(tempDir.c_str(), targetDir.c_str(), 0)) { ok = true; break; }
				code = GetLastError();
				if (code != ERROR_ACCESS_DENIED && code != ERROR_SHARING_VIOLATION) break;
			}
			if (!ok) {
				result.error = (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS)
					? "The destination folder already exists. Nothing was changed."
					: "Could not move the extracted folder to its destination" + Code(code) + ".";
			}
		}

		if (!ok) {
			if (tempCreated) SafePath::RemoveTree(tempDir);
			result.files = 0;
			result.bytes = 0;
		}
		return ok;
	}
}
