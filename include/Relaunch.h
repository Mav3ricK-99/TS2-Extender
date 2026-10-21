#pragma once
#include <string>

namespace Relaunch {
	// Only "-load:\"<text>\"" is accepted. <text> must be valid UTF-8 with no double quotes or control characters.
	bool ValidateArgs(const std::string& utf8Args, std::string& error);

	// Starts a detached helper that waits for this game process to exit and then starts the same executable again
	// with the given arguments. It does NOT close the game. If the game is still running after timeoutSeconds, nothing is launched.
	bool Arm(const std::string& utf8Args, int timeoutSeconds, std::string& error);

	// Presses Alt+F4 so the player can close the game quickly. Only sends the keys if the foreground window belongs
	// to this process (otherwise they would close some other application). Returns whether the keys were sent.
	bool SendAltF4();
}
