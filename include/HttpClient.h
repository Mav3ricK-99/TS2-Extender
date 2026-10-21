#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <Windows.h>

// Minimal blocking HTTP/HTTPS client on top of WinHTTP (not WinINet: Extender replaces wininet.dll).
// It streams the body to disk in chunks, hashes it on the fly, and never touches Lua or the game.
// Call it from a worker thread, never from the game thread.
namespace Http {
	struct Options {
		uint64_t maxBytes = 1024ull * 1024 * 1024;  // The download is aborted if the body is bigger than this.
		DWORD totalTimeoutMs = 10 * 60 * 1000;      // Whole request, from start to last byte.
		DWORD connectTimeoutMs = 15 * 1000;
		DWORD ioTimeoutMs = 30 * 1000;              // Send / receive timeout of each step.
		int maxRedirects = 5;                       // 0 = do not follow redirects (Request uses it for methods with a body).
	};

	struct Result {
		int status = 0;         // Final HTTP status, 0 if there was no response.
		uint64_t bytes = 0;     // Bytes written to disk.
		std::string sha256;     // Lowercase hex digest of the body (set when the body was read completely).
		std::string body;       // Response body, kept in memory. Only Request fills it, even when it returns false with a status.
		std::string error;      // Set when the call returns false.
	};

	// True for exactly 64 hexadecimal characters.
	bool IsSha256Hex(const std::string& text);

	// GET `url` (http or https, UTF-8) and save the body to `destPath`.
	// The data goes to "<destPath>.part" and is only renamed over `destPath` once the whole body arrived and,
	// if `expectedSha256` is not empty, matched it. On any failure the partial file is deleted and `destPath` is untouched.
	// `cancel` is polled between chunks; the call returns false with "Cancelled." soon after it becomes true.
	bool GetToFile(const std::string& url, const std::wstring& destPath, const std::string& expectedSha256,
		const Options& options, const std::atomic<bool>& cancel, Result& result);

	// True for a method this client sends: GET, HEAD, POST, PUT, PATCH, DELETE or OPTIONS (upper case).
	bool IsAllowedMethod(const std::string& method);

	// Checks request headers given as text, one "Name: value" per line (\n or \r\n), and writes them to `normalized`
	// as "Name: value\r\n" lines. Rejects invalid names or values, more than 32 lines, 8 KB of text, and the headers
	// that the client or the connection manage by themselves (Host, Content-Length, Transfer-Encoding, Connection...).
	bool NormalizeHeaders(const std::string& text, std::string& normalized, std::string& error);

	// Sends `method` to `url` with an optional `body` and `headers` (as returned by NormalizeHeaders) and keeps the
	// answer in `result.body` (at most `options.maxBytes`). True only for a 2xx status. When the server answered with
	// another status, it returns false with `result.status` and `result.body` set, so the caller can read the reason.
	bool Request(const std::string& method, const std::string& url, const std::string& body, const std::string& headers,
		const Options& options, const std::atomic<bool>& cancel, Result& result);

	// POSTs the file at `filePath` as the request body, streamed from disk in chunks (never all in memory). `headers` is
	// as returned by NormalizeHeaders (e.g. the Content-Type). Redirects are not followed. The size is limited by
	// `options.maxBytes` and, because WinHTTP takes the length as a DWORD, by 4 GB. Like Request, it keeps the answer in
	// `result.body` and is true only for a 2xx status; on another status it returns false with `status` and `body` set.
	// `result.bytes` is the number of bytes of the file that were sent.
	bool PostFile(const std::string& url, const std::wstring& filePath, const std::string& headers,
		const Options& options, const std::atomic<bool>& cancel, Result& result);
}
