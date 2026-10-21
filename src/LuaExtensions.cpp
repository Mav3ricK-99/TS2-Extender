#include "LuaExtensions.h"
#include "ts2/cIGZLua5Thread.h"
#include "MinHook.h"
#include "Addresses.h"
#include "Logging.h"
#include "ts2/CheatSystem.h"
#include "LuaCheatCommand.h"
#include "PatchVersion.h"
#include "ts2/cUserInput.h"
#include "ts2/nRZSceneGraph.h"
#include "Core.h"
#include "Utils.h"
#include <filesystem>
#include <algorithm>
#include <fstream>
#include "scan.h"
#include <Windows.h>
#include "ts2/cGZFramework.h"
#include "ts2/RZ.h"
#include "ts2/cIGZWin.h"
#include "Relaunch.h"
#include "Network.h"
#include <vector>
#include "config.h"
#define KEY_COUNT 256

namespace LuaExtensions {
	typedef bool(__cdecl* REGISTERPRIMITIVESUPPORTLUACOMMANDS)(TS2::cIGZLua5Thread*);
	static REGISTERPRIMITIVESUPPORTLUACOMMANDS fpRegisterPrimitiveSupportLuaCommands = NULL;
	typedef void(__stdcall* REGISTERTSSGCHEATS)();
	static REGISTERTSSGCHEATS fpRegisterTSSGCheats = NULL;

	static SHORT currentInputState[KEY_COUNT]{};
	static SHORT lastInputState[KEY_COUNT]{};

	// Calls the Lua callbacks of the finished DownloadFile / ExtractNeighborhood / ZipNeighborhood / UploadNeighborhood / HttpRequest jobs.
	// Runs on the game thread (FrameUpdate), the only place where network results may touch Lua.
	static void DeliverNetworkResults() {
		std::vector<Network::Completed> completed;
		Network::TakeCompleted(completed);
		for (Network::Completed& result : completed) {
			lua_State* luaState = result.luaState;
			// The Lua state was recreated while the job was running: its callback no longer exists.
			if (result.ownerState != Core::_instance->m_LuaState) {
				Log("Network: dropped the result of a job started in a previous Lua state.\n");
				continue;
			}
			lua_rawgeti(luaState, LUA_REGISTRYINDEX, result.callbackRef);
			lua_pushboolean(luaState, result.ok ? 1 : 0);
			if (result.ok) {
				// There is no lua_pushnil wrapper: growing the stack with lua_settop fills the new slot with nil.
				lua_settop(luaState, lua_gettop(luaState) + 1);
			}
			else {
				lua_pushstring(luaState, result.error.c_str());
			}
			int argCount = 3;
			if (result.isRequest) {
				// HttpRequest and UploadNeighborhood: callback(ok, err, status, response). status is 0 when the server never answered.
				lua_pushnumber(luaState, static_cast<double>(result.status));
				lua_pushstring(luaState, result.response.c_str());
				argCount = 4;
			}
			else {
				lua_pushnumber(luaState, static_cast<double>(result.bytes));
			}
			if (lua_pcall(luaState, argCount, 0, 0) != 0) {
				Log("Error calling Lua network callback: %s\n", lua_tostring(luaState, -1));
				lua_pop(luaState, 1);
			}
			luaL_unref(luaState, LUA_REGISTRYINDEX, result.callbackRef);
		}
	}

	void FrameUpdate() {
		memcpy(lastInputState, currentInputState, sizeof(currentInputState));
		for (int i = 0; i < KEY_COUNT; i++) {
			currentInputState[i] = GetAsyncKeyState(i);
		}
		DeliverNetworkResults();
	}

	static std::string GetProcessDirectory() {
		wchar_t path[MAX_PATH];
		if (GetModuleFileNameW(NULL, path, MAX_PATH)) {
			std::string dir = WCharToString(path);
			size_t pos = dir.find_last_of("\\/");
			if (pos != std::string::npos) {
				return dir.substr(0, pos);
			}
		}
		return "";
	}

	// MAT_RemoveVariable(string name)
	static int __cdecl LuaMatRemoveVariable(lua_State* luaState) {
		const char* varName = lua_tostring(luaState, 1);
		nRZSceneGraph::MaterialParser()->RemoveVariable(0, varName);
		return 0;
	}

	// MAT_SetVariable(string name, string value)
	static int __cdecl LuaMatSetVariable(lua_State* luaState) {
		const char* varName = lua_tostring(luaState, 1);
		const char* varValue = lua_tostring(luaState, 2);
		nRZSceneGraph::MaterialParser()->SetVariable(0, varName, varValue);
		return 0;
	}

	// ExecuteCommand(string cheat)
	static int __cdecl LuaExecuteCommand(lua_State* luaState) {
		const char* cmd = lua_tostring(luaState, 1);
		TS2::CheatSystem()->ExecuteCommand(cmd);
		return 0;
	}

	// IsLegacy()
	static int __cdecl LuaIsLegacy(lua_State* luaState) {
#if TS2_LC
		lua_pushboolean(luaState, 1);
#else
		lua_pushboolean(luaState, 0);
#endif
		return 1;
	}

	static bool CanTakeInput() {
		HWND win;
		cIGZApp* app = nullptr;
		if (RZGetFramework()->QueryInterface(IID_GZAPP, (cIGZUnknown**)&app))
		{
			win = app->GetMainHWND();
			app->Release();
		}
		else {
			return false;
		}
		if (win == nullptr) return false;
		if (GetForegroundWindow() != win) return false;
		return true;
	}

	// UI_IsBusy()
	static int __cdecl LuaUIIsBusy(lua_State* luaState) {
		if (!CanTakeInput())
		{
			lua_pushboolean(luaState, 1);
			return 1;
		}
		cGZWinMgrBase* wm = RZ::WM();
		if (wm == nullptr) {
			lua_pushboolean(luaState, 1);
			return 1;
		}
		if (wm->IsModal()) {
			lua_pushboolean(luaState, 1);
			return 1;
		}
		
		cIGZWin* focusedWin = wm->GZGetFocus();
		if (focusedWin != nullptr) {
			cIGZUnknown* winInterface;
			if (focusedWin->QueryInterface(IID_GZWINTEXTEDIT, &winInterface)) {
				winInterface->Release();
				lua_pushboolean(luaState, 1);
				return 1;
			}
		}
		lua_pushboolean(luaState, 0);
		return 1;
	}

	// KBM_GetKeyDown(number vk)
	static int __cdecl LuaGetKeyDown(lua_State* luaState) {
		if (!CanTakeInput())
		{
			lua_pushboolean(luaState, 0);
			return 1;
		}
		int vk = static_cast<int>(lua_tonumber(luaState, 1));
		bool res = (currentInputState[vk] & 0x8000) && !(lastInputState[vk] & 0x8000);
		lua_pushboolean(luaState, res ? 1 : 0);
		return 1;
	}

	// KBM_GetKey(number vk)
	static int __cdecl LuaGetKeyHeld(lua_State* luaState) {
		if (!CanTakeInput())
		{
			lua_pushboolean(luaState, 0);
			return 1;
		}
		int vk = static_cast<int>(lua_tonumber(luaState, 1));
		bool res = (currentInputState[vk] & 0x8000);
		lua_pushboolean(luaState, res ? 1 : 0);
		return 1;
	}

	// KBM_GetKeyUp(number vk)
	static int __cdecl LuaGetKeyUp(lua_State* luaState) {
		if (!CanTakeInput())
		{
			lua_pushboolean(luaState, 0);
			return 1;
		}
		int vk = static_cast<int>(lua_tonumber(luaState, 1));
		bool res = !(currentInputState[vk] & 0x8000) && (lastInputState[vk] & 0x8000);
		lua_pushboolean(luaState, res ? 1 : 0);
		return 1;
	}

	// GetCASLot()
	static int __cdecl LuaGetCASLot(lua_State* luaState) {
		lua_pushstring(luaState, Addresses::CASLotName);
		return 1;
	}

	// GetYACASLot()
	static int __cdecl LuaGetYACASLot(lua_State* luaState) {
		lua_pushstring(luaState, Addresses::YACASLotName);
		return 1;
	}

	// SetCASLot(string lot)
	static int __cdecl LuaSetCASLot(lua_State* luaState) {
		const char* lot = lua_tostring(luaState, 1);
		size_t lotLen = std::strlen(lot);
		if (lotLen > 7) {
			Log("Lua: SetCASLot FAILED! Lot name must be 7 characters or less.\n");
			return 0;
		}
		WriteToMemory((DWORD)Addresses::CASLotName, (void*)lot, lotLen + 1);
		return 0;
	}

	// SetYACASLot(string lot)
	static int __cdecl LuaSetYACASLot(lua_State* luaState) {
		const char* lot = lua_tostring(luaState, 1);
		size_t lotLen = std::strlen(lot);
		if (lotLen > 7) {
			Log("Lua: SetYACASLot FAILED! Lot name must be 7 characters or less.\n");
			return 0;
		}
		WriteToMemory((DWORD)Addresses::YACASLotName, (void*)lot, lotLen + 1);
		return 0;
	}

	// EnsureDirectory(string path)
	static int __cdecl LuaEnsureDirectory(lua_State* luaState) {
		const char* path = lua_tostring(luaState, 1);

		std::filesystem::path fPath = std::filesystem::u8path(path);

		std::filesystem::create_directories(fPath);
		return 0;
	}

	// {string, string...} GetFilesInDirectory(string path, string extension)
	static int __cdecl LuaGetFilesInDirectory(lua_State* luaState) {
		const char* path = lua_tostring(luaState, 1);
		const char* ext = lua_tostring(luaState, 2);

		std::string checkExtension = "";
		if (ext != nullptr)
			checkExtension = std::string(ext);

		std::filesystem::path fPath = std::filesystem::u8path(path);

		std::transform(checkExtension.begin(), checkExtension.end(), checkExtension.begin(), ::tolower);

		lua_newtable(luaState);

		int index = 1;

		for (const auto& entry : std::filesystem::recursive_directory_iterator(fPath)) {
			if (entry.is_regular_file()) {
				std::string fext = entry.path().extension().u8string();
				std::transform(fext.begin(), fext.end(), fext.begin(), ::tolower);

				if (fext == checkExtension || checkExtension == "")
				{
					lua_pushnumber(luaState, index);
					lua_pushstring(luaState, entry.path().u8string().c_str());
					lua_settable(luaState, -3);
					index++;
				}
			}
		}

		return 1;
	}

	static int __cdecl LuaRegisterCheat(lua_State* luaState) {
		const char* cheatName = lua_tostring(luaState, 1);
		const char* cheatDesc = lua_tostring(luaState, 2);
		lua_pushvalue(luaState, 3);
		int executeRef = luaL_ref(luaState, LUA_REGISTRYINDEX);
		LuaCheatCommand* luaCheat = new LuaCheatCommand(cheatName, cheatDesc, executeRef, luaState);
		TS2::cTSCheatSystem* cheatSystem = TS2::CheatSystem();
		cheatSystem->RegisterCheat(luaCheat);
		return 0;
	}

	static int __cdecl LuaGetUserDirectory(lua_State* luaState) {
		std::string utf8Dir = WCharToString(Core::_instance->m_UserDataPath.c_str());
		lua_pushstring(luaState, utf8Dir.c_str());
		return 1;
	}

	static int __cdecl LuaGetExecutableDirectory(lua_State* luaState) {
		lua_pushstring(luaState, GetProcessDirectory().c_str());
		return 1;
	}

	static int __cdecl LuaGetTS2ExtenderVersion(lua_State* luaState) {
		lua_pushstring(luaState, Version);
		return 1;
	}

	static int __cdecl LuaGetUserInput(lua_State* luaState) {
		cUserInput* userInput = cUserInput::GetInstance();
		if (userInput == nullptr)
		{
			lua_pushstring(luaState, "");
			return 1;
		}
		const char* userString = userInput->GetString();
		if (userString == nullptr) {
			lua_pushstring(luaState, "");
			return 1;
		}
		lua_pushstring(luaState, userString);
		return 1;
	}

	// OverrideString(originalindex, originalinstance, originalgroup, repindex, repinstance, repgroup)
	static int __cdecl LuaOverrideString(lua_State* luaState) {

		uint16_t oIndex = static_cast<uint16_t>(lua_tonumber(luaState, 1));
		uint16_t oInstance = static_cast<uint16_t>(lua_tonumber(luaState, 2));
		uint32_t oGroup = static_cast<uint32_t>(lua_tonumber(luaState, 3));

		uint16_t nIndex = static_cast<uint16_t>(lua_tonumber(luaState, 4));
		uint16_t nInstance = static_cast<uint16_t>(lua_tonumber(luaState, 5));
		uint32_t nGroup = static_cast<uint32_t>(lua_tonumber(luaState, 6));

		Core::_instance->m_StringOverrides[StringId(oIndex, oInstance, oGroup)] = StringId(nIndex, nInstance, nGroup);
		return 0;
	}

	// ClearStringOverride(originalindex, originalinstance, originalgroup)
	static int __cdecl LuaClearStringOverride(lua_State* luaState) {

		uint16_t oIndex = static_cast<uint16_t>(lua_tonumber(luaState, 1));
		uint16_t oInstance = static_cast<uint16_t>(lua_tonumber(luaState, 2));
		uint32_t oGroup = static_cast<uint32_t>(lua_tonumber(luaState, 3));

		StringId strId = StringId(oIndex, oInstance, oGroup);
		auto it = Core::_instance->m_StringOverrides.find(strId);

		if (it != Core::_instance->m_StringOverrides.end()) {
			Core::_instance->m_StringOverrides.erase(it);
		}
		return 0;
	}

	// OverrideUI(originalInstance, newInstance)
	static int __cdecl LuaOverrideUI(lua_State* luaState) {
		uint32_t oUI = static_cast<uint32_t>(lua_tonumber(luaState, 1));

		uint32_t nUI = static_cast<uint32_t>(lua_tonumber(luaState, 2));

		Core::_instance->m_UIOverrides[oUI] = nUI;
		return 0;
	}

	// ClearUIOverride(originalInstance)
	static int __cdecl LuaClearUIOverride(lua_State* luaState) {
		uint32_t oUI = static_cast<uint32_t>(lua_tonumber(luaState, 1));

		auto it = Core::_instance->m_UIOverrides.find(oUI);

		if (it != Core::_instance->m_UIOverrides.end()) {
			Core::_instance->m_UIOverrides.erase(it);
		}
		return 0;
	}

	// OverrideMakeMoneyString(function)
	// function return: string
	// function params: money (number)
	static int __cdecl LuaOverrideMakeMoneyString(lua_State* luaState) {
		lua_pushvalue(luaState, 1);
		int executeRef = luaL_ref(luaState, LUA_REGISTRYINDEX);
		Core::_instance->m_MakeMoneyStringLuaCall = executeRef;
		return 0;
	}

	static int __cdecl LuaClearMakeMoneyStringOverride(lua_State* luaState) {
		Core::_instance->m_MakeMoneyStringLuaCall = LUA_NOREF;
		return 0;
	}

	// string id AddGameCallback(number delegate, function callback, number priority)
	static int __cdecl LuaAddGameCallback(lua_State* luaState) {
		int delegate = lua_tonumber(luaState, 1);
		int priority = lua_tonumber(luaState, 3);
		lua_pushvalue(luaState, 2);
		int executeRef = luaL_ref(luaState, LUA_REGISTRYINDEX);
		Id64 result = Core::_instance->m_LuaDelegates[delegate].AddCallback({ executeRef, luaState, priority });
		lua_pushstring(luaState, std::to_string(result.m_Value).c_str());
		return 1;
	}

	// RemoveGameCallback(number delegate, string id)
	static int __cdecl LuaRemoveGameCallback(lua_State* luaState) {
		int delegate = lua_tonumber(luaState, 1);
		std::string idString = lua_tostring(luaState, 2);
		Id64 callbackId = Id64(static_cast<uint64_t>(std::stoll(idString, nullptr)));
		Core::_instance->m_LuaDelegates[delegate].RemoveCallback(callbackId);
		return 0;
	}

	// string ReadFile(string path)
	static int __cdecl LuaReadFile(lua_State* luaState) {
		const char* path = lua_tostring(luaState, 1);
		std::filesystem::path fPath = std::filesystem::u8path(path);

		if (!std::filesystem::exists(fPath)) {
			lua_pushstring(luaState, "");
			return 1;
		}

		std::ifstream file(fPath, std::ios::in | std::ios::binary);

		if (!file)
			throw std::runtime_error("Failed to open file!");

		std::string fstr = std::string(
			std::istreambuf_iterator<char>(file),
			std::istreambuf_iterator<char>()
		);

		file.close();

		lua_pushstring(luaState, fstr.c_str());
		return 1;
	}

	// WriteFile(string path, string contents)
	static int __cdecl LuaWriteFile(lua_State* luaState) {
		const char* path = lua_tostring(luaState, 1);
		const char* content = lua_tostring(luaState, 2);

		std::filesystem::path fPath = std::filesystem::u8path(path);

		std::ofstream file(path, std::ios::binary | std::ios::out | std::ios::trunc);

		if (!file)
			throw std::runtime_error("Failed to open file!");

		file.write(content, strlen(content));

		file.close();
		return 0;
	}

	// boolean ok, string err RelaunchGame(string args)
	// Arms a restart of the game with the given launch args, e.g. -load:"N001;165 Sim Lane".
	// Presses Alt+F4 (if the game window is focused) so the player can close the game quickly; the restart
	// happens once the game exits.
	static int __cdecl LuaRelaunchGame(lua_State* luaState) {
		if (!Config::EnableNetworkApi) {
			lua_pushboolean(luaState, 0);
			lua_pushstring(luaState, "RelaunchGame is disabled. Set EnableNetworkApi = true in TS2Extender.ini.");
			return 2;
		}
		const char* args = lua_tostring(luaState, 1);
		if (args == nullptr) {
			lua_pushboolean(luaState, 0);
			lua_pushstring(luaState, "args must be a string.");
			return 2;
		}
		std::string error;
		bool ok = false;
		try {
			ok = Relaunch::Arm(args, Config::RelaunchCloseTimeoutSeconds, error);
		}
		catch (const std::exception& e) {
			error = e.what();
		}
		if (!ok) {
			Log("Lua: RelaunchGame FAILED! %s\n", error.c_str());
			lua_pushboolean(luaState, 0);
			lua_pushstring(luaState, error.c_str());
			return 2;
		}
		Relaunch::SendAltF4();
		lua_pushboolean(luaState, 1);
		return 1;
	}

	static int PushFailure(lua_State* luaState, const char* function, const std::string& message) {
		Log("Lua: %s FAILED! %s\n", function, message.c_str());
		lua_pushboolean(luaState, 0);
		lua_pushstring(luaState, message.c_str());
		return 2;
	}

	// Takes the callback at stack index `idx` as a registry reference. Returns LUA_NOREF if it can't be a function.
	// The Lua wrappers have no lua_type, so only nil, strings and numbers are detected; anything else that
	// isn't callable just fails inside lua_pcall when the result is delivered.
	static int RefCallback(lua_State* luaState, int idx) {
		if (lua_gettop(luaState) < idx || lua_tostring(luaState, idx) != nullptr)
			return LUA_NOREF;
		lua_pushvalue(luaState, idx);
		int ref = luaL_ref(luaState, LUA_REGISTRYINDEX);
		return ref < 0 ? LUA_NOREF : ref;
	}

	// boolean ok, string err DownloadFile(string url, string destPath, function callback [, string expectedSha256])
	// Starts an HTTP(S) GET in a background thread and returns at once. destPath must be inside the staging
	// folder (<GetUserDirectory()>\TS2ExtenderStaging). Later, from the game thread, callback(ok, err, bytes) is called.
	static int __cdecl LuaDownloadFile(lua_State* luaState) {
		if (!Config::EnableNetworkApi)
			return PushFailure(luaState, "DownloadFile", "DownloadFile is disabled. Set EnableNetworkApi = true in TS2Extender.ini.");
		const char* url = lua_tostring(luaState, 1);
		const char* destPath = lua_tostring(luaState, 2);
		if (url == nullptr || destPath == nullptr)
			return PushFailure(luaState, "DownloadFile", "url and destPath must be strings.");
		const char* sha256 = lua_gettop(luaState) >= 4 ? lua_tostring(luaState, 4) : nullptr;
		int ref = RefCallback(luaState, 3);
		if (ref == LUA_NOREF)
			return PushFailure(luaState, "DownloadFile", "callback must be a function.");

		std::string error;
		bool ok = false;
		try {
			ok = Network::QueueDownload(luaState, ref, url, destPath, sha256 != nullptr ? sha256 : "", error);
		}
		catch (const std::exception& e) {
			error = e.what();
		}
		if (!ok) {
			luaL_unref(luaState, LUA_REGISTRYINDEX, ref);
			return PushFailure(luaState, "DownloadFile", error);
		}
		lua_pushboolean(luaState, 1);
		return 1;
	}

	// boolean ok, string err ExtractNeighborhood(string zipPath, string extractDir, function callback)
	// Extracts a .zip from the staging folder into a NEW folder directly inside Neighborhoods, in a background
	// thread. Nothing is ever overwritten: if extractDir exists, the callback gets ok=false.
	// Later, from the game thread, callback(ok, err, bytes) is called; bytes is the uncompressed size written.
	static int __cdecl LuaExtractNeighborhood(lua_State* luaState) {
		if (!Config::EnableNetworkApi)
			return PushFailure(luaState, "ExtractNeighborhood", "ExtractNeighborhood is disabled. Set EnableNetworkApi = true in TS2Extender.ini.");
		const char* zipPath = lua_tostring(luaState, 1);
		const char* extractDir = lua_tostring(luaState, 2);
		if (zipPath == nullptr || extractDir == nullptr)
			return PushFailure(luaState, "ExtractNeighborhood", "zipPath and extractDir must be strings.");
		int ref = RefCallback(luaState, 3);
		if (ref == LUA_NOREF)
			return PushFailure(luaState, "ExtractNeighborhood", "callback must be a function.");

		std::string error;
		bool ok = false;
		try {
			ok = Network::QueueExtract(luaState, ref, zipPath, extractDir, error);
		}
		catch (const std::exception& e) {
			error = e.what();
		}
		if (!ok) {
			luaL_unref(luaState, LUA_REGISTRYINDEX, ref);
			return PushFailure(luaState, "ExtractNeighborhood", error);
		}
		lua_pushboolean(luaState, 1);
		return 1;
	}

	// boolean ok, string err ZipNeighborhood(string name, string zipDir, function callback)
	// Zips the neighborhood folder `name` (for example "N001") from Neighborhoods into <zipDir>\<name>.zip, in a
	// background thread. zipDir must be inside the staging folder. The live neighborhood is only read: it is first copied
	// to <zipDir>\Temp, zipped from there, and the copy is deleted. Later, from the game thread,
	// callback(ok, err, bytes) is called; bytes is the size of the .zip.
	static int __cdecl LuaZipNeighborhood(lua_State* luaState) {
		if (!Config::EnableNetworkApi)
			return PushFailure(luaState, "ZipNeighborhood", "ZipNeighborhood is disabled. Set EnableNetworkApi = true in TS2Extender.ini.");
		const char* name = lua_tostring(luaState, 1);
		const char* zipDir = lua_tostring(luaState, 2);
		if (name == nullptr || zipDir == nullptr)
			return PushFailure(luaState, "ZipNeighborhood", "name and zipDir must be strings.");
		int ref = RefCallback(luaState, 3);
		if (ref == LUA_NOREF)
			return PushFailure(luaState, "ZipNeighborhood", "callback must be a function.");

		std::string error;
		bool ok = false;
		try {
			ok = Network::QueueZip(luaState, ref, name, zipDir, error);
		}
		catch (const std::exception& e) {
			error = e.what();
		}
		if (!ok) {
			luaL_unref(luaState, LUA_REGISTRYINDEX, ref);
			return PushFailure(luaState, "ZipNeighborhood", error);
		}
		lua_pushboolean(luaState, 1);
		return 1;
	}

	// boolean ok, string err UploadNeighborhood(string url, string zipPath, function callback)
	// Uploads a .zip from the staging folder with an HTTP POST (the raw file as the body, Content-Type application/zip),
	// in a background thread. url is a full http(s) URL or a path starting with "/", which goes to ServerUrl.
	// Later, from the game thread, callback(ok, err, status, response) is called, like HttpRequest's: ok is true for a 2xx
	// status, status is the HTTP status (0 if the server never answered) and response is the body of the answer.
	static int __cdecl LuaUploadNeighborhood(lua_State* luaState) {
		if (!Config::EnableNetworkApi)
			return PushFailure(luaState, "UploadNeighborhood", "UploadNeighborhood is disabled. Set EnableNetworkApi = true in TS2Extender.ini.");
		const char* url = lua_tostring(luaState, 1);
		const char* zipPath = lua_tostring(luaState, 2);
		if (url == nullptr || zipPath == nullptr)
			return PushFailure(luaState, "UploadNeighborhood", "url and zipPath must be strings.");
		int ref = RefCallback(luaState, 3);
		if (ref == LUA_NOREF)
			return PushFailure(luaState, "UploadNeighborhood", "callback must be a function.");

		std::string error;
		bool ok = false;
		try {
			ok = Network::QueueUpload(luaState, ref, url, zipPath, error);
		}
		catch (const std::exception& e) {
			error = e.what();
		}
		if (!ok) {
			luaL_unref(luaState, LUA_REGISTRYINDEX, ref);
			return PushFailure(luaState, "UploadNeighborhood", error);
		}
		lua_pushboolean(luaState, 1);
		return 1;
	}

	// boolean ok, string err HttpRequest(string method, string url, string body, string headers, function callback)
	// Sends an HTTP request in a background thread and returns at once. method is GET, HEAD, POST, PUT, PATCH, DELETE
	// or OPTIONS. url is a full http(s) URL or a path starting with "/", which goes to ServerUrl. body and headers
	// may be nil; headers is text with one "Name: value" per line. Later, from the game thread,
	// callback(ok, err, status, response) is called: ok is true for a 2xx status, status is the HTTP status
	// (0 if the server never answered) and response is the body of the answer.
	static int __cdecl LuaHttpRequest(lua_State* luaState) {
		if (!Config::EnableNetworkApi)
			return PushFailure(luaState, "HttpRequest", "HttpRequest is disabled. Set EnableNetworkApi = true in TS2Extender.ini.");
		const char* method = lua_tostring(luaState, 1);
		const char* url = lua_tostring(luaState, 2);
		if (method == nullptr || url == nullptr)
			return PushFailure(luaState, "HttpRequest", "method and url must be strings.");
		const char* body = lua_tostring(luaState, 3);
		const char* headers = lua_tostring(luaState, 4);
		int ref = RefCallback(luaState, 5);
		if (ref == LUA_NOREF)
			return PushFailure(luaState, "HttpRequest", "callback must be a function (pass nil for body or headers you don't use).");

		std::string error;
		bool ok = false;
		try {
			ok = Network::QueueRequest(luaState, ref, method, url, body != nullptr ? body : "", headers != nullptr ? headers : "", error);
		}
		catch (const std::exception& e) {
			error = e.what();
		}
		if (!ok) {
			luaL_unref(luaState, LUA_REGISTRYINDEX, ref);
			return PushFailure(luaState, "HttpRequest", error);
		}
		lua_pushboolean(luaState, 1);
		return 1;
	}

	static int __cdecl LuaSetLoadUIScriptDebug(lua_State* luaState) {
		Core::_instance->m_LoadUIScriptDebug = lua_toboolean(luaState, 1) != 0;
		return 0;
	}

	static bool __cdecl DetourRegisterPrimitiveSupportLuaCommands(TS2::cIGZLua5Thread* luaThread) {
		Core::_instance->m_LuaState = luaThread->GetLuaState();
		bool res = fpRegisterPrimitiveSupportLuaCommands(luaThread);
		if (luaThread != NULL) {
			luaThread->Register(&LuaGetExecutableDirectory, "GetExecutableDirectory");
			luaThread->Register(&LuaGetUserDirectory, "GetUserDirectory");
			luaThread->Register(&LuaRegisterCheat, "RegisterCheat");
			luaThread->Register(&LuaGetTS2ExtenderVersion, "GetTS2ExtenderVersion");
			luaThread->Register(&LuaGetUserInput, "GetUserInput");
			luaThread->Register(&LuaOverrideString, "OverrideString");
			luaThread->Register(&LuaClearStringOverride, "ClearStringOverride");
			luaThread->Register(&LuaOverrideUI, "OverrideUI");
			luaThread->Register(&LuaClearUIOverride, "ClearUIOverride");
			luaThread->Register(&LuaOverrideMakeMoneyString, "OverrideMakeMoneyString");
			luaThread->Register(&LuaClearMakeMoneyStringOverride, "ClearMakeMoneyStringOverride");
			luaThread->Register(&LuaAddGameCallback, "AddGameCallback");
			luaThread->Register(&LuaRemoveGameCallback, "RemoveGameCallback");
			luaThread->Register(&LuaEnsureDirectory, "EnsureDirectory");
			luaThread->Register(&LuaGetFilesInDirectory, "GetFilesInDirectory");
			luaThread->Register(&LuaReadFile, "ReadFile");
			luaThread->Register(&LuaWriteFile, "WriteFile");
			luaThread->Register(&LuaSetLoadUIScriptDebug, "SetLoadUIScriptDebug");
			luaThread->Register(&LuaSetCASLot, "SetCASLot");
			luaThread->Register(&LuaSetYACASLot, "SetYACASLot");
			luaThread->Register(&LuaGetCASLot, "GetCASLot");
			luaThread->Register(&LuaGetYACASLot, "GetYACASLot");
			luaThread->Register(&LuaGetKeyDown, "KBM_GetKeyDown");
			luaThread->Register(&LuaGetKeyHeld, "KBM_GetKey");
			luaThread->Register(&LuaGetKeyUp, "KBM_GetKeyUp");
			luaThread->Register(&LuaIsLegacy, "IsLegacy");
			luaThread->Register(&LuaExecuteCommand, "ExecuteCommand");
			luaThread->Register(&LuaMatSetVariable, "MAT_SetVariable");
			luaThread->Register(&LuaMatRemoveVariable, "MAT_RemoveVariable");
			luaThread->Register(&LuaUIIsBusy, "UI_IsBusy");
			luaThread->Register(&LuaRelaunchGame, "RelaunchGame");
			luaThread->Register(&LuaDownloadFile, "DownloadFile");
			luaThread->Register(&LuaExtractNeighborhood, "ExtractNeighborhood");
			luaThread->Register(&LuaZipNeighborhood, "ZipNeighborhood");
			luaThread->Register(&LuaUploadNeighborhood, "UploadNeighborhood");
			luaThread->Register(&LuaHttpRequest, "HttpRequest");
		}
		return res;
	}

	static void __stdcall DetourRegisterTSSGCheats() {
		Log("Register TSSG cheats!\n");
		fpRegisterTSSGCheats();
	}

	bool Initialize() {
		if (MH_CreateHook(Addresses::RegisterLuaCommands, &DetourRegisterPrimitiveSupportLuaCommands,
			reinterpret_cast<LPVOID*>(&fpRegisterPrimitiveSupportLuaCommands)) != MH_OK)
		{
			Log("LuaExtensions PrimitiveSupportLuaCommands patch failed!\n");
			return false;
		}
		if (MH_EnableHook(Addresses::RegisterLuaCommands) != MH_OK)
		{
			Log("LuaExtensions PrimitiveSupportLuaCommands patch failed!\n");
			return false;
		}
		if (MH_CreateHook(Addresses::RegisterTSSGCheats, &DetourRegisterTSSGCheats,
			reinterpret_cast<LPVOID*>(&fpRegisterTSSGCheats)) != MH_OK)
		{
			Log("LuaExtensions RegisterTSSGCheats patch failed!\n");
			return false;
		}
		if (MH_EnableHook(Addresses::RegisterTSSGCheats) != MH_OK)
		{
			Log("LuaExtensions RegisterTSSGCheats patch failed!\n");
			return false;
		}
		return true;
	}
}