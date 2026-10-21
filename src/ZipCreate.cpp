#include "ZipCreate.h"
#include "SafePath.h"
#include "miniz.h"
#include <Windows.h>
#include <algorithm>
#include <vector>

namespace ZipCreate {
	namespace {
		struct Entry {
			bool isDirectory;
			std::wstring path;      // Relative to the neighborhood folder, with backslashes.
			uint64_t size;
		};

		struct HandleCloser {
			HANDLE h = INVALID_HANDLE_VALUE;
			~HandleCloser() { Close(); }
			void Close() { if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); h = INVALID_HANDLE_VALUE; } }
		};

		std::string Code(DWORD code) {
			return " (error " + std::to_string(code) + ")";
		}

		bool Exists(const std::wstring& path) {
			return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
		}

		std::wstring ParentOf(const std::wstring& path) {
			size_t slash = path.find_last_of(L'\\');
			return slash == std::wstring::npos ? L"" : path.substr(0, slash);
		}

		std::string ToUtf8(const std::wstring& s) {
			if (s.empty()) return "";
			int count = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
			std::string out(count, '\0');
			WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), count, nullptr, nullptr);
			return out;
		}

		// Everything the copy step needs to know and to report.
		struct CopyState {
			std::vector<Entry>* entries;
			const Limits* limits;
			const std::atomic<bool>* cancel;
			uint64_t total = 0;
			std::string error;
		};

		bool CopyFileContents(const std::wstring& from, const std::wstring& to, CopyState& state, uint64_t& copied) {
			HandleCloser in;
			// The game may have the file open: share everything so reading doesn't fail because of it.
			in.h = CreateFileW(from.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
				OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
			if (in.h == INVALID_HANDLE_VALUE) {
				state.error = "Could not read a neighborhood file" + Code(GetLastError()) + ".";
				return false;
			}
			HandleCloser out;
			// CREATE_NEW: nothing is ever overwritten, not even inside the temporary folder.
			out.h = CreateFileW(to.c_str(), GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
			if (out.h == INVALID_HANDLE_VALUE) {
				state.error = "Could not create a temporary file" + Code(GetLastError()) + ".";
				return false;
			}
			std::vector<char> buffer(1u << 20);
			copied = 0;
			while (true) {
				if (state.cancel->load()) { state.error = "Cancelled."; return false; }
				DWORD got = 0;
				if (!ReadFile(in.h, buffer.data(), static_cast<DWORD>(buffer.size()), &got, NULL)) {
					state.error = "Could not read a neighborhood file" + Code(GetLastError()) + ".";
					return false;
				}
				if (got == 0) break;
				// Counted as it is read: the game may still be growing a file.
				if (state.total + got > state.limits->maxTotalBytes) {
					state.error = "The neighborhood is too big to zip.";
					return false;
				}
				DWORD wrote = 0;
				if (!WriteFile(out.h, buffer.data(), got, &wrote, NULL) || wrote != got) {
					state.error = "Could not write a temporary file (disk full?)" + Code(GetLastError()) + ".";
					return false;
				}
				copied += got;
				state.total += got;
			}
			return true;
		}

		// Copies `from` into the already created folder `to`, recording what was copied in order (folders before their contents).
		bool CopyTree(const std::wstring& from, const std::wstring& to, const std::wstring& relative, CopyState& state) {
			WIN32_FIND_DATAW data;
			HANDLE find = FindFirstFileW((from + L"\\*").c_str(), &data);
			if (find == INVALID_HANDLE_VALUE) {
				DWORD code = GetLastError();
				if (code == ERROR_FILE_NOT_FOUND) return true;
				state.error = "Could not list a neighborhood folder" + Code(code) + ".";
				return false;
			}
			bool ok = true;
			do {
				if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L"..")) continue;
				if (state.cancel->load()) { state.error = "Cancelled."; ok = false; break; }

				bool isDirectory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
				// Symbolic links and junctions are refused (cloud placeholders such as OneDrive's are other reparse tags and are fine).
				if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
					(data.dwReserved0 == IO_REPARSE_TAG_SYMLINK || data.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT)) {
					state.error = "The neighborhood contains a symbolic link or junction.";
					ok = false;
					break;
				}

				std::wstring name = data.cFileName;
				std::wstring source = from + L"\\" + name;
				std::wstring target = to + L"\\" + name;
				std::wstring path = relative.empty() ? name : relative + L"\\" + name;
				if (target.size() > SafePath::MaxPathLength) { state.error = "The neighborhood has a path that is too long."; ok = false; break; }
				if (state.entries->size() >= state.limits->maxEntries) { state.error = "The neighborhood has too many files."; ok = false; break; }

				if (isDirectory) {
					if (!CreateDirectoryW(target.c_str(), NULL)) {
						state.error = "Could not create a temporary folder" + Code(GetLastError()) + ".";
						ok = false;
						break;
					}
					state.entries->push_back({ true, path, 0 });
					if (!CopyTree(source, target, path, state)) { ok = false; break; }
				}
				else {
					uint64_t copied = 0;
					if (!CopyFileContents(source, target, state, copied)) { ok = false; break; }
					state.entries->push_back({ false, path, copied });
				}
			} while (FindNextFileW(find, &data));
			FindClose(find);
			return ok;
		}

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

		// The .zip being written. miniz hands over the offset of every write; it is almost always the end of the file.
		struct ZipOutput {
			HANDLE file;
			uint64_t position;
		};

		size_t WriteToZip(void* opaque, mz_uint64 offset, const void* buffer, size_t size) {
			ZipOutput* output = static_cast<ZipOutput*>(opaque);
			if (offset != output->position) {
				LARGE_INTEGER seek;
				seek.QuadPart = static_cast<LONGLONG>(offset);
				if (!SetFilePointerEx(output->file, seek, NULL, FILE_BEGIN))
					return 0;
				output->position = offset;
			}
			size_t done = 0;
			while (done < size) {
				DWORD chunk = static_cast<DWORD>(std::min<size_t>(size - done, 1u << 20));
				DWORD wrote = 0;
				if (!WriteFile(output->file, static_cast<const char*>(buffer) + done, chunk, &wrote, NULL) || wrote != chunk)
					return 0;
				done += wrote;
			}
			output->position += size;
			return size;
		}

		// Compresses the copy in `tempDir` into the open `zipFile`. Writes nothing outside of it.
		bool WriteArchive(const std::vector<Entry>& entries, const std::wstring& tempDir, HANDLE zipFile,
			const std::atomic<bool>& cancel, std::string& error) {
			ZipOutput output{ zipFile, 0 };
			mz_zip_archive zip;
			mz_zip_zero_struct(&zip);
			zip.m_pWrite = WriteToZip;
			zip.m_pIO_opaque = &output;
			if (!mz_zip_writer_init(&zip, 0)) {
				error = std::string("Could not start the zip: ") + mz_zip_get_error_string(mz_zip_get_last_error(&zip)) + ".";
				return false;
			}

			bool ok = true;
			for (const Entry& e : entries) {
				if (cancel.load()) { error = "Cancelled."; ok = false; break; }

				std::wstring name = e.path;
				std::replace(name.begin(), name.end(), L'\\', L'/');
				std::string utf8 = ToUtf8(name);
				if (e.isDirectory) {
					utf8 += '/';
					if (!mz_zip_writer_add_mem(&zip, utf8.c_str(), "", 0, MZ_NO_COMPRESSION)) {
						error = std::string("Could not add a folder to the zip: ") + mz_zip_get_error_string(mz_zip_get_last_error(&zip)) + ".";
						ok = false;
						break;
					}
					continue;
				}

				HandleCloser in;
				in.h = CreateFileW((tempDir + L"\\" + e.path).c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
				if (in.h == INVALID_HANDLE_VALUE) {
					error = "Could not read a temporary file" + Code(GetLastError()) + ".";
					ok = false;
					break;
				}
				if (!mz_zip_writer_add_read_buf_callback(&zip, utf8.c_str(), ReadFromFile, in.h, e.size, nullptr, nullptr, 0,
					MZ_DEFAULT_LEVEL, nullptr, 0, nullptr, 0)) {
					error = std::string("Could not add a file to the zip: ") + mz_zip_get_error_string(mz_zip_get_last_error(&zip)) + ".";
					ok = false;
					break;
				}
			}
			if (ok && !mz_zip_writer_finalize_archive(&zip)) {
				error = std::string("Could not finish the zip: ") + mz_zip_get_error_string(mz_zip_get_last_error(&zip)) + ".";
				ok = false;
			}
			mz_zip_writer_end(&zip);
			return ok;
		}
	}

	bool CreateFromDirectory(const std::wstring& sourceDir, const std::wstring& tempDir, const std::wstring& zipPath,
		const Limits& limits, const std::atomic<bool>& cancel, Result& result) {
		result = Result{};

		DWORD sourceAttrs = GetFileAttributesW(sourceDir.c_str());
		if (sourceAttrs == INVALID_FILE_ATTRIBUTES || !(sourceAttrs & FILE_ATTRIBUTE_DIRECTORY)) {
			result.error = "The neighborhood folder was not found.";
			return false;
		}
		if (Exists(tempDir)) { result.error = "The temporary folder already exists."; return false; }
		DWORD parentAttrs = GetFileAttributesW(ParentOf(zipPath).c_str());
		if (parentAttrs == INVALID_FILE_ATTRIBUTES || !(parentAttrs & FILE_ATTRIBUTE_DIRECTORY)) {
			result.error = "The destination folder of the zip does not exist.";
			return false;
		}

		std::wstring partPath = zipPath + L".part";
		bool tempCreated = false;
		bool partCreated = false;
		bool ok = false;
		try {
			if (!CreateDirectoryW(tempDir.c_str(), NULL)) {
				result.error = "Could not create the temporary folder" + Code(GetLastError()) + ".";
			}
			else {
				tempCreated = true;
				std::vector<Entry> entries;
				CopyState state{ &entries, &limits, &cancel };
				if (!CopyTree(sourceDir, tempDir, L"", state)) {
					result.error = state.error;
				}
				else if (entries.empty()) {
					result.error = "The neighborhood folder is empty.";
				}
				else {
					HandleCloser zipFile;
					// Our own staging area: a leftover .part from an interrupted run is simply replaced.
					zipFile.h = CreateFileW(partPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
					if (zipFile.h == INVALID_HANDLE_VALUE) {
						result.error = "Could not create the zip file" + Code(GetLastError()) + ".";
					}
					else {
						partCreated = true;
						if (WriteArchive(entries, tempDir, zipFile.h, cancel, result.error)) {
							LARGE_INTEGER size;
							if (GetFileSizeEx(zipFile.h, &size)) result.bytes = static_cast<uint64_t>(size.QuadPart);
							for (const Entry& e : entries)
								if (!e.isDirectory) result.files++;
							ok = true;
						}
						zipFile.Close();
					}
				}
			}
		}
		catch (const std::exception& e) {
			result.error = std::string("Unexpected error: ") + e.what();
			ok = false;
		}

		// The copy is never kept.
		if (tempCreated) SafePath::RemoveTree(tempDir);

		if (ok) {
			// Antivirus or indexers may hold the new file for a moment. The previous zip, if any, is replaced.
			DWORD code = 0;
			ok = false;
			for (int attempt = 0; attempt < 10 && !ok; attempt++) {
				if (attempt > 0) Sleep(200);
				if (MoveFileExW(partPath.c_str(), zipPath.c_str(), MOVEFILE_REPLACE_EXISTING)) { ok = true; break; }
				code = GetLastError();
				if (code != ERROR_ACCESS_DENIED && code != ERROR_SHARING_VIOLATION) break;
			}
			if (!ok) result.error = "Could not move the zip to its destination" + Code(code) + ".";
		}

		if (!ok) {
			if (partCreated) DeleteFileW(partPath.c_str());
			result.files = 0;
			result.bytes = 0;
		}
		return ok;
	}
}
