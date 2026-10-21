# RelaunchGame

`boolean ok, string err RelaunchGame(string args)`

Arms a restart of the game with the given launch arguments, for example to load a lot directly. Right after arming it, it presses **Alt+F4** so the player can close the game quickly; when the game exits, it is started again with `args`. The Sims 2 can't run two instances at once, so the new one is only launched after the current process has exited.

The function never kills the process. Alt+F4 is a normal close request, so the game (or the player, if the game asks for confirmation) decides whether it closes. The keys are only sent if the game window has the focus; otherwise nothing is pressed (so no other application gets closed) and the player has to close the game by hand. The relaunch stays armed either way.

Draft in Markdown, to be adapted to the format of the TS2 Extender docs site (separate repo).

## Parameters

| Name | Type | Description |
|---|---|---|
| `args` | string | Launch arguments. Only `-load:"<text>"` is accepted, for example `-load:"N001;165 Sim Lane"`. `<text>` must be valid UTF-8 without double quotes or control characters, and the whole string at most 512 bytes. |

## Returns

* `true` when the restart has been armed.
* `false, err` when it hasn't, with a message in `err`: the option is disabled, `args` is invalid, a relaunch is already pending, or the helper process couldn't be started.

## Requirements

Disabled by default. Enable it in `TS2Extender.ini`:

```ini
[Network]
EnableNetworkApi = true
; Seconds to wait for the player to close the game before cancelling the relaunch.
RelaunchCloseTimeoutSeconds = 300
```

If the game is still running when `RelaunchCloseTimeoutSeconds` elapses, nothing is launched.

## Example

```lua
local ok, err = RelaunchGame('-load:"N001;165 Sim Lane"')
if not ok then
	print("Could not arm the relaunch: " .. err)
else
	-- Alt+F4 has been pressed; the game restarts by itself once it closes.
end
```

## Notes

* The executable that is relaunched is always the running game's own executable; it can't be configured from Lua.
* The wait is done by a short-lived `powershell.exe` started with a fixed script. Values are passed through environment variables, never inside the script text.
* Not available on Wine/Linux (needs PowerShell).
* Antivirus software may flag the use of PowerShell by the `.asi`.
