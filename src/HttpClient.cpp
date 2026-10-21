#include "HttpClient.h"
#include <winhttp.h>
#include <bcrypt.h>
#include <algorithm>
#include <vector>

namespace Http {
	namespace {
		const wchar_t* UserAgent = L"TS2Extender";
		const DWORD ChunkSize = 64 * 1024;

		struct WinHttpHandle {
			HINTERNET h = nullptr;
			WinHttpHandle() = default;
			explicit WinHttpHandle(HINTERNET handle) : h(handle) {}
			WinHttpHandle(const WinHttpHandle&) = delete;
			WinHttpHandle& operator=(const WinHttpHandle&) = delete;
			~WinHttpHandle() { if (h) WinHttpCloseHandle(h); }
		};

		struct FileHandle {
			HANDLE h = INVALID_HANDLE_VALUE;
			~FileHandle() { Close(); }
			void Close() { if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); h = INVALID_HANDLE_VALUE; } }
		};

		// SHA-256 through BCrypt (part of Windows, no extra dependency).
		class Sha256 {
		public:
			Sha256() {
				if (BCryptOpenAlgorithmProvider(&m_alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0) { m_alg = nullptr; return; }
				if (BCryptCreateHash(m_alg, &m_hash, NULL, 0, NULL, 0, 0) != 0) m_hash = nullptr;
			}
			~Sha256() {
				if (m_hash) BCryptDestroyHash(m_hash);
				if (m_alg) BCryptCloseAlgorithmProvider(m_alg, 0);
			}
			Sha256(const Sha256&) = delete;
			Sha256& operator=(const Sha256&) = delete;
			bool Valid() const { return m_hash != nullptr; }
			bool Update(const void* data, size_t size) {
				return BCryptHashData(m_hash, static_cast<PUCHAR>(const_cast<void*>(data)), static_cast<ULONG>(size), 0) == 0;
			}
			bool Finish(std::string& hex) {
				UCHAR digest[32];
				if (BCryptFinishHash(m_hash, digest, sizeof(digest), 0) != 0) return false;
				static const char digits[] = "0123456789abcdef";
				hex.clear();
				for (UCHAR b : digest) {
					hex.push_back(digits[b >> 4]);
					hex.push_back(digits[b & 0xF]);
				}
				return true;
			}
		private:
			BCRYPT_ALG_HANDLE m_alg = nullptr;
			BCRYPT_HASH_HANDLE m_hash = nullptr;
		};

		std::wstring Widen(const std::string& utf8) {
			int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
			if (count <= 0) return {};
			std::wstring result(count, L'\0');
			MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(), static_cast<int>(utf8.size()), result.data(), count);
			return result;
		}

		std::string WinHttpErrorText(DWORD code) {
			switch (code) {
			case ERROR_WINHTTP_TIMEOUT: return "The connection timed out.";
			case ERROR_WINHTTP_NAME_NOT_RESOLVED: return "The host name could not be resolved.";
			case ERROR_WINHTTP_CANNOT_CONNECT: return "Could not connect to the server.";
			case ERROR_WINHTTP_CONNECTION_ERROR: return "The connection to the server was lost.";
			case ERROR_WINHTTP_SECURE_FAILURE:
			case ERROR_WINHTTP_SECURE_INVALID_CA:
			case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
			case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
			case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
			case ERROR_WINHTTP_SECURE_INVALID_CERT:
				return "The TLS certificate of the server was rejected.";
			case ERROR_WINHTTP_REDIRECT_FAILED: return "Too many redirects, or a redirect from https to http.";
			case ERROR_WINHTTP_INVALID_URL: return "The URL is not valid.";
			default: return "Network error " + std::to_string(code) + ".";
			}
		}

		bool Fail(Result& result, const std::string& message) {
			result.error = message;
			return false;
		}

		bool FailWin(Result& result, const char* what, DWORD code) {
			return Fail(result, std::string(what) + ": " + WinHttpErrorText(code));
		}

		uint64_t ParseUnsigned(const std::wstring& text) {
			uint64_t value = 0;
			for (wchar_t c : text) {
				if (c < L'0' || c > L'9') return UINT64_MAX;
				if (value > (UINT64_MAX - 9) / 10) return UINT64_MAX;
				value = value * 10 + (c - L'0');
			}
			return text.empty() ? UINT64_MAX : value;
		}
	}

	bool IsSha256Hex(const std::string& text) {
		if (text.size() != 64) return false;
		for (char c : text) {
			bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
			if (!hex) return false;
		}
		return true;
	}

	// Session, connection and request handles of one call. Declared in this order so they close in reverse.
	struct Connection {
		WinHttpHandle session;
		WinHttpHandle connection;
		WinHttpHandle request;
	};

	// Parses `url` and opens (without sending) a request with `method` on `c`. With maxRedirects == 0 redirects are not
	// followed, which POST needs: WinHTTP would turn the redirected POST into a GET and drop the body.
	static bool OpenRequest(const wchar_t* method, const std::string& url, const Options& options, Connection& c, Result& result) {
		if (url.empty() || url.size() > 2048) return Fail(result, "The URL is empty or too long.");
		std::wstring wideUrl = Widen(url);
		if (wideUrl.empty()) return Fail(result, "The URL is not valid UTF-8.");

		URL_COMPONENTSW parts{};
		parts.dwStructSize = sizeof(parts);
		parts.dwSchemeLength = parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength =
			parts.dwUserNameLength = parts.dwPasswordLength = static_cast<DWORD>(-1);
		if (!WinHttpCrackUrl(wideUrl.c_str(), static_cast<DWORD>(wideUrl.size()), 0, &parts))
			return Fail(result, "The URL is not valid.");
		bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
		if (!secure && parts.nScheme != INTERNET_SCHEME_HTTP)
			return Fail(result, "Only http and https URLs are supported.");
		if (parts.dwUserNameLength != 0 || parts.dwPasswordLength != 0)
			return Fail(result, "URLs with credentials are not supported.");
		if (parts.dwHostNameLength == 0)
			return Fail(result, "The URL has no host.");

		std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
		std::wstring object(parts.lpszUrlPath, parts.dwUrlPathLength);
		object.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
		if (object.empty()) object = L"/";

		// Automatic proxy detection needs Windows 8.1; older systems fall back to the machine proxy settings.
		c.session.h = WinHttpOpen(UserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		if (!c.session.h)
			c.session.h = WinHttpOpen(UserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		if (!c.session.h) return FailWin(result, "Could not start WinHTTP", GetLastError());

		WinHttpSetTimeouts(c.session.h, static_cast<int>(options.connectTimeoutMs), static_cast<int>(options.connectTimeoutMs),
			static_cast<int>(options.ioTimeoutMs), static_cast<int>(options.ioTimeoutMs));
		if (secure) {
			// TLS 1.2 | 1.3. Older systems reject the 1.3 flag, so retry with 1.2 only; ignore a second failure (system default).
			DWORD protocols = 0x00000800 | 0x00002000;
			if (!WinHttpSetOption(c.session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
				protocols = 0x00000800;
				WinHttpSetOption(c.session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
			}
		}

		c.connection.h = WinHttpConnect(c.session.h, host.c_str(), parts.nPort, 0);
		if (!c.connection.h) return FailWin(result, "Could not connect", GetLastError());

		c.request.h = WinHttpOpenRequest(c.connection.h, method, object.c_str(), NULL, WINHTTP_NO_REFERER,
			WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
		if (!c.request.h) return FailWin(result, "Could not create the request", GetLastError());

		// Redirects are followed by WinHTTP; https -> http downgrades stay blocked (the default policy).
		if (options.maxRedirects > 0) {
			DWORD maxRedirects = static_cast<DWORD>(options.maxRedirects);
			WinHttpSetOption(c.request.h, WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS, &maxRedirects, sizeof(maxRedirects));
		}
		else {
			DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
			WinHttpSetOption(c.request.h, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
		}
		return true;
	}

	static bool Download(const std::string& url, const std::wstring& partPath, const std::string& expectedSha256,
		const Options& options, const std::atomic<bool>& cancel, Result& result) {
		ULONGLONG deadline = GetTickCount64() + options.totalTimeoutMs;
		auto timedOut = [&]() { return GetTickCount64() >= deadline; };

		Connection c;
		if (!OpenRequest(L"GET", url, options, c, result)) return false;
		WinHttpHandle& request = c.request;

		if (cancel.load()) return Fail(result, "Cancelled.");
		if (!WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
			return FailWin(result, "Could not send the request", GetLastError());
		if (!WinHttpReceiveResponse(request.h, NULL))
			return FailWin(result, "No response", GetLastError());

		DWORD status = 0;
		DWORD statusSize = sizeof(status);
		if (!WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX))
			return FailWin(result, "Could not read the response", GetLastError());
		result.status = static_cast<int>(status);
		if (status != 200)
			return Fail(result, "The server answered HTTP " + std::to_string(status) + ".");

		wchar_t lengthText[32];
		DWORD lengthSize = sizeof(lengthText);
		if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, lengthText, &lengthSize, WINHTTP_NO_HEADER_INDEX)) {
			uint64_t declared = ParseUnsigned(lengthText);
			if (declared != UINT64_MAX && declared > options.maxBytes)
				return Fail(result, "The file is bigger than the allowed download size.");
		}

		Sha256 hash;
		if (!hash.Valid()) return Fail(result, "Could not start the SHA-256 hash.");

		FileHandle file;
		file.h = CreateFileW(partPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (file.h == INVALID_HANDLE_VALUE)
			return Fail(result, "Could not create the destination file (error " + std::to_string(GetLastError()) + ").");

		std::vector<char> buffer(ChunkSize);
		while (true) {
			if (cancel.load()) return Fail(result, "Cancelled.");
			if (timedOut()) return Fail(result, "The download took too long.");

			DWORD read = 0;
			if (!WinHttpReadData(request.h, buffer.data(), ChunkSize, &read))
				return FailWin(result, "The download was interrupted", GetLastError());
			if (read == 0) break;

			if (result.bytes + read > options.maxBytes)
				return Fail(result, "The file is bigger than the allowed download size.");
			if (!hash.Update(buffer.data(), read))
				return Fail(result, "SHA-256 failed.");
			DWORD written = 0;
			if (!WriteFile(file.h, buffer.data(), read, &written, NULL) || written != read)
				return Fail(result, "Could not write the file (error " + std::to_string(GetLastError()) + "). Is the disk full?");
			result.bytes += read;
		}

		if (!FlushFileBuffers(file.h))
			return Fail(result, "Could not flush the file (error " + std::to_string(GetLastError()) + ").");
		file.Close();

		if (!hash.Finish(result.sha256)) return Fail(result, "SHA-256 failed.");
		if (!expectedSha256.empty()) {
			std::string expected = expectedSha256;
			for (char& c : expected)
				if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
			if (expected != result.sha256)
				return Fail(result, "SHA-256 mismatch: the downloaded file is not the expected one.");
		}
		return true;
	}

	// Reads the status and the body of the answer into `result` (up to options.maxBytes). True only for a 2xx status.
	// The body is read even for an error status: the server may explain it there. Used once WinHttpSendRequest finished.
	static bool ReadAnswer(WinHttpHandle& request, ULONGLONG deadline, const Options& options, const std::atomic<bool>& cancel, Result& result) {
		if (!WinHttpReceiveResponse(request.h, NULL))
			return FailWin(result, "No response", GetLastError());

		DWORD status = 0;
		DWORD statusSize = sizeof(status);
		if (!WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX))
			return FailWin(result, "Could not read the response", GetLastError());
		result.status = static_cast<int>(status);

		std::vector<char> buffer(ChunkSize);
		while (true) {
			if (cancel.load()) return Fail(result, "Cancelled.");
			if (GetTickCount64() >= deadline) return Fail(result, "The request took too long.");
			DWORD read = 0;
			if (!WinHttpReadData(request.h, buffer.data(), ChunkSize, &read))
				return FailWin(result, "The response was interrupted", GetLastError());
			if (read == 0) break;
			if (result.body.size() + read > options.maxBytes)
				return Fail(result, "The response is bigger than the allowed size.");
			result.body.append(buffer.data(), read);
		}
		result.bytes = result.body.size();

		if (status < 200 || status >= 300)
			return Fail(result, "The server answered HTTP " + std::to_string(status) + ".");
		return true;
	}

	static bool Run(const std::string& method, const std::string& url, const std::string& body, const std::string& headers,
		const Options& options, const std::atomic<bool>& cancel, Result& result) {
		if (body.size() > 0x7FFFFFFF) return Fail(result, "The request body is too big.");
		std::wstring wideMethod = Widen(method);
		std::wstring wideHeaders = Widen(headers);
		if (wideMethod.empty()) return Fail(result, "The method is not valid.");
		ULONGLONG deadline = GetTickCount64() + options.totalTimeoutMs;

		Connection c;
		if (!OpenRequest(wideMethod.c_str(), url, options, c, result)) return false;

		if (cancel.load()) return Fail(result, "Cancelled.");
		DWORD length = static_cast<DWORD>(body.size());
		if (!WinHttpSendRequest(c.request.h,
				wideHeaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : wideHeaders.c_str(),
				wideHeaders.empty() ? 0 : static_cast<DWORD>(wideHeaders.size()),
				body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()), length, length, 0))
			return FailWin(result, "Could not send the request", GetLastError());
		return ReadAnswer(c.request, deadline, options, cancel, result);
	}

	// POSTs the file at `filePath` as the request body, read from disk in chunks (the game is 32-bit: never all in memory).
	static bool Upload(const std::string& url, const std::wstring& filePath, const std::string& headers,
		const Options& options, const std::atomic<bool>& cancel, Result& result) {
		std::wstring wideHeaders = Widen(headers);
		ULONGLONG deadline = GetTickCount64() + options.totalTimeoutMs;

		// Deny writers while it is sent, so the size announced in Content-Length stays true.
		FileHandle file;
		file.h = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (file.h == INVALID_HANDLE_VALUE)
			return Fail(result, "Could not open the file to upload (error " + std::to_string(GetLastError()) + ").");
		LARGE_INTEGER size{};
		if (!GetFileSizeEx(file.h, &size))
			return Fail(result, "Could not read the size of the file (error " + std::to_string(GetLastError()) + ").");
		if (size.QuadPart <= 0) return Fail(result, "The file to upload is empty.");
		// WinHttpSendRequest takes the total length as a DWORD.
		if (static_cast<uint64_t>(size.QuadPart) > options.maxBytes || static_cast<uint64_t>(size.QuadPart) > 0xFFFFFFFFull)
			return Fail(result, "The file is bigger than the allowed upload size.");
		DWORD total = static_cast<DWORD>(size.QuadPart);

		Connection c;
		if (!OpenRequest(L"POST", url, options, c, result)) return false;
		WinHttpHandle& request = c.request;

		if (cancel.load()) return Fail(result, "Cancelled.");
		if (!WinHttpSendRequest(request.h,
				wideHeaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : wideHeaders.c_str(),
				wideHeaders.empty() ? 0 : static_cast<DWORD>(wideHeaders.size()),
				WINHTTP_NO_REQUEST_DATA, 0, total, 0))
			return FailWin(result, "Could not send the request", GetLastError());

		std::vector<char> buffer(ChunkSize);
		uint64_t sent = 0;
		while (sent < total) {
			if (cancel.load()) return Fail(result, "Cancelled.");
			if (GetTickCount64() >= deadline) return Fail(result, "The upload took too long.");

			DWORD toRead = static_cast<DWORD>((std::min<uint64_t>)(ChunkSize, total - sent));
			DWORD read = 0;
			if (!ReadFile(file.h, buffer.data(), toRead, &read, NULL))
				return Fail(result, "Could not read the file to upload (error " + std::to_string(GetLastError()) + ").");
			if (read == 0) return Fail(result, "The file to upload changed while it was being sent.");
			DWORD written = 0;
			if (!WinHttpWriteData(request.h, buffer.data(), read, &written) || written != read)
				return FailWin(result, "The upload was interrupted", GetLastError());
			sent += read;
		}
		file.Close();
		bool ok = ReadAnswer(request, deadline, options, cancel, result);
		result.bytes = sent;     // Bytes uploaded (ReadAnswer left the size of the answer here).
		return ok;
	}

	bool IsAllowedMethod(const std::string& method) {
		static const char* const allowed[] = { "GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS" };
		for (const char* name : allowed)
			if (method == name) return true;
		return false;
	}

	bool NormalizeHeaders(const std::string& text, std::string& normalized, std::string& error) {
		normalized.clear();
		if (text.size() > 8192) { error = "headers are bigger than 8 KB."; return false; }
		// Headers the client or the connection manage by themselves: letting a mod set them could break the framing.
		static const char* const reserved[] = { "host", "content-length", "transfer-encoding", "connection", "upgrade", "expect", "te", "trailer" };
		int count = 0;
		size_t pos = 0;
		while (pos <= text.size()) {
			size_t end = text.find('\n', pos);
			if (end == std::string::npos) end = text.size();
			std::string line = text.substr(pos, end - pos);
			pos = end + 1;
			if (!line.empty() && line.back() == '\r') line.pop_back();
			if (line.empty()) continue;

			size_t colon = line.find(':');
			if (colon == std::string::npos || colon == 0) { error = "each header must look like \"Name: value\"."; return false; }
			std::string name = line.substr(0, colon);
			std::string value = line.substr(colon + 1);
			for (char ch : name) {
				bool token = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
					(ch != 0 && strchr("!#$%&'*+-.^_`|~", ch) != nullptr);
				if (!token) { error = "header name \"" + name + "\" has invalid characters."; return false; }
			}
			size_t first = value.find_first_not_of(" \t");
			size_t last = value.find_last_not_of(" \t");
			value = first == std::string::npos ? std::string() : value.substr(first, last - first + 1);
			for (char ch : value) {
				unsigned char u = static_cast<unsigned char>(ch);
				if ((u < 0x20 && u != '\t') || u > 0x7E) { error = "the value of header \"" + name + "\" has control or non-ASCII characters."; return false; }
			}
			std::string lower = name;
			for (char& ch : lower) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
			for (const char* r : reserved)
				if (lower == r) { error = "header \"" + name + "\" cannot be set."; return false; }
			if (++count > 32) { error = "more than 32 headers."; return false; }
			normalized += name + ": " + value + "\r\n";
		}
		return true;
	}

	bool Request(const std::string& method, const std::string& url, const std::string& body, const std::string& headers,
		const Options& options, const std::atomic<bool>& cancel, Result& result) {
		result = Result{};
		if (!IsAllowedMethod(method)) {
			result.error = "The method must be GET, HEAD, POST, PUT, PATCH, DELETE or OPTIONS.";
			return false;
		}
		bool ok = false;
		try {
			ok = Run(method, url, body, headers, options, cancel, result);
		}
		catch (const std::exception& e) {
			result.error = std::string("Unexpected error: ") + e.what();
		}
		return ok;
	}

	bool PostFile(const std::string& url, const std::wstring& filePath, const std::string& headers,
		const Options& options, const std::atomic<bool>& cancel, Result& result) {
		result = Result{};
		bool ok = false;
		try {
			ok = Upload(url, filePath, headers, options, cancel, result);
		}
		catch (const std::exception& e) {
			result.error = std::string("Unexpected error: ") + e.what();
		}
		return ok;
	}

	bool GetToFile(const std::string& url, const std::wstring& destPath, const std::string& expectedSha256,
		const Options& options, const std::atomic<bool>& cancel, Result& result) {
		result = Result{};
		if (!expectedSha256.empty() && !IsSha256Hex(expectedSha256)) {
			result.error = "The expected SHA-256 must be 64 hexadecimal characters.";
			return false;
		}
		std::wstring partPath = destPath + L".part";
		bool ok = false;
		try {
			ok = Download(url, partPath, expectedSha256, options, cancel, result);
		}
		catch (const std::exception& e) {
			result.error = std::string("Unexpected error: ") + e.what();
		}
		if (ok && !MoveFileExW(partPath.c_str(), destPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
			result.error = "Could not move the file to its destination (error " + std::to_string(GetLastError()) + ").";
			ok = false;
		}
		if (!ok) {
			DeleteFileW(partPath.c_str());
			result.bytes = 0;
		}
		return ok;
	}
}
