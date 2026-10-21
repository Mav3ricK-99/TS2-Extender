#include "Relaunch.h"
#include "Logging.h"
#include <Windows.h>
#include <vector>

namespace Relaunch {
	static const size_t MaxArgsLength = 512;
	static const int MinTimeoutSeconds = 5;
	static const int MaxTimeoutSeconds = 24 * 60 * 60;

	// Handle of the helper PowerShell process, so only one relaunch can be pending at a time.
	static HANDLE pendingProcess = nullptr;

	// Fixed script. The values it uses are passed through environment variables of the helper process,
	// never spliced into the script text.
	static const wchar_t* HelperScript =
		L"$ErrorActionPreference = 'Stop'\n"
		L"$p = Get-Process -Id ([int]$env:TS2E_PID) -ErrorAction SilentlyContinue\n"
		L"if ($p -and -not $p.WaitForExit([int]$env:TS2E_TIMEOUT_MS)) { exit 1 }\n"
		L"Start-Process -FilePath $env:TS2E_EXE -WorkingDirectory $env:TS2E_DIR -ArgumentList $env:TS2E_ARGS\n";

	bool ValidateArgs(const std::string& args, std::string& error) {
		static const std::string prefix = "-load:\"";
		if (args.empty()) {
			error = "args is empty.";
			return false;
		}
		if (args.size() > MaxArgsLength) {
			error = "args is too long.";
			return false;
		}
		if (args.size() < prefix.size() + 2 || args.compare(0, prefix.size(), prefix) != 0 || args.back() != '"') {
			error = "Only -load:\"<lot>\" is accepted as args.";
			return false;
		}
		for (size_t i = prefix.size(); i < args.size() - 1; i++) {
			unsigned char c = static_cast<unsigned char>(args[i]);
			if (c == '"') {
				error = "args has an unexpected double quote.";
				return false;
			}
			if (c < 0x20 || c == 0x7F) {
				error = "args has a control character.";
				return false;
			}
		}
		if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, args.c_str(), -1, nullptr, 0) <= 0) {
			error = "args is not valid UTF-8.";
			return false;
		}
		return true;
	}

	static std::wstring Widen(const std::string& utf8) {
		int count = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
		if (count <= 1)
			return {};
		std::wstring result(count, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, result.data(), count);
		result.pop_back();
		return result;
	}

	static std::string Base64(const unsigned char* data, size_t len) {
		static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		std::string out;
		out.reserve(((len + 2) / 3) * 4);
		for (size_t i = 0; i < len; i += 3) {
			unsigned int v = data[i] << 16;
			if (i + 1 < len) v |= data[i + 1] << 8;
			if (i + 2 < len) v |= data[i + 2];
			out.push_back(table[(v >> 18) & 0x3F]);
			out.push_back(table[(v >> 12) & 0x3F]);
			out.push_back(i + 1 < len ? table[(v >> 6) & 0x3F] : '=');
			out.push_back(i + 2 < len ? table[v & 0x3F] : '=');
		}
		return out;
	}

	// Copy of the current environment block plus the extra "NAME=value" entries.
	static std::vector<wchar_t> BuildEnvironment(const std::vector<std::wstring>& extra) {
		std::vector<wchar_t> block;
		LPWCH env = GetEnvironmentStringsW();
		if (env != nullptr) {
			const wchar_t* p = env;
			while (*p != L'\0')
				p += wcslen(p) + 1;
			block.assign(static_cast<const wchar_t*>(env), p);
			FreeEnvironmentStringsW(env);
		}
		for (const std::wstring& entry : extra) {
			block.insert(block.end(), entry.begin(), entry.end());
			block.push_back(L'\0');
		}
		block.push_back(L'\0');
		return block;
	}

	static std::string ErrorText(const char* what, DWORD code) {
		return std::string(what) + " (error " + std::to_string(code) + ").";
	}

	bool SendAltF4() {
		// The keys go to whatever window has the focus, so only send them if that is the game's.
		HWND foreground = GetForegroundWindow();
		DWORD foregroundPid = 0;
		if (foreground != nullptr)
			GetWindowThreadProcessId(foreground, &foregroundPid);
		if (foregroundPid != GetCurrentProcessId()) {
			Log("Relaunch: the game window isn't focused, Alt+F4 not sent.\n");
			return false;
		}

		INPUT inputs[4]{};
		const WORD keys[4] = { VK_MENU, VK_F4, VK_F4, VK_MENU };
		for (int i = 0; i < 4; i++) {
			inputs[i].type = INPUT_KEYBOARD;
			inputs[i].ki.wVk = keys[i];
			inputs[i].ki.wScan = static_cast<WORD>(MapVirtualKeyW(keys[i], MAPVK_VK_TO_VSC));
			if (i >= 2)
				inputs[i].ki.dwFlags = KEYEVENTF_KEYUP;
		}
		if (SendInput(4, inputs, sizeof(INPUT)) != 4) {
			Log("Relaunch: SendInput failed (error %lu), Alt+F4 not sent.\n", GetLastError());
			return false;
		}
		Log("Relaunch: Alt+F4 sent.\n");
		return true;
	}

	bool Arm(const std::string& args, int timeoutSeconds, std::string& error) {
		if (!ValidateArgs(args, error))
			return false;

		if (pendingProcess != nullptr) {
			if (WaitForSingleObject(pendingProcess, 0) == WAIT_TIMEOUT) {
				error = "A relaunch is already pending.";
				return false;
			}
			CloseHandle(pendingProcess);
			pendingProcess = nullptr;
		}

		std::vector<wchar_t> exeBuffer(32768);
		DWORD exeLen = GetModuleFileNameW(NULL, exeBuffer.data(), static_cast<DWORD>(exeBuffer.size()));
		if (exeLen == 0 || exeLen >= exeBuffer.size()) {
			error = ErrorText("Could not get the game executable path", GetLastError());
			return false;
		}
		std::wstring exePath(exeBuffer.data(), exeLen);
		size_t slash = exePath.find_last_of(L"\\/");
		std::wstring exeDir = slash == std::wstring::npos ? L"" : exePath.substr(0, slash);

		wchar_t systemDir[MAX_PATH];
		UINT systemDirLen = GetSystemDirectoryW(systemDir, MAX_PATH);
		if (systemDirLen == 0 || systemDirLen >= MAX_PATH) {
			error = ErrorText("Could not get the system directory", GetLastError());
			return false;
		}
		std::wstring powershell = std::wstring(systemDir) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
		if (GetFileAttributesW(powershell.c_str()) == INVALID_FILE_ATTRIBUTES) {
			error = "powershell.exe was not found.";
			return false;
		}

		if (timeoutSeconds < MinTimeoutSeconds) timeoutSeconds = MinTimeoutSeconds;
		if (timeoutSeconds > MaxTimeoutSeconds) timeoutSeconds = MaxTimeoutSeconds;

		std::vector<wchar_t> environment = BuildEnvironment({
			L"TS2E_PID=" + std::to_wstring(GetCurrentProcessId()),
			L"TS2E_TIMEOUT_MS=" + std::to_wstring(timeoutSeconds * 1000),
			L"TS2E_EXE=" + exePath,
			L"TS2E_DIR=" + exeDir,
			L"TS2E_ARGS=" + Widen(args)
		});

		std::wstring script = HelperScript;
		std::string encoded = Base64(reinterpret_cast<const unsigned char*>(script.data()), script.size() * sizeof(wchar_t));
		std::wstring commandLine = L"\"" + powershell + L"\" -NoProfile -NonInteractive -EncodedCommand " + Widen(encoded);

		STARTUPINFOW si{};
		si.cb = sizeof(si);
		PROCESS_INFORMATION pi{};
		DWORD flags = CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT;

		// Try to leave the game's job object (if any) so the helper isn't killed together with the game.
		BOOL created = CreateProcessW(powershell.c_str(), commandLine.data(), NULL, NULL, FALSE,
			flags | CREATE_BREAKAWAY_FROM_JOB, environment.data(), exeDir.c_str(), &si, &pi);
		if (!created && GetLastError() == ERROR_ACCESS_DENIED) {
			created = CreateProcessW(powershell.c_str(), commandLine.data(), NULL, NULL, FALSE,
				flags, environment.data(), exeDir.c_str(), &si, &pi);
		}
		if (!created) {
			error = ErrorText("Could not start powershell.exe", GetLastError());
			return false;
		}

		CloseHandle(pi.hThread);
		pendingProcess = pi.hProcess;
		Log("Relaunch armed. The game will restart once it is closed (timeout %d seconds).\n", timeoutSeconds);
		return true;
	}
}
