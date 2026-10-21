# ExtractNeighborhood

`boolean ok, string err ExtractNeighborhood(string zipPath, string extractDir, function callback)`

Unzips a `.zip` into a **new** folder inside `Neighborhoods`. It never replaces or modifies anything that already exists: if `extractDir` exists, nothing is done and the callback reports the error. The work runs in a background thread; the function returns immediately and `callback` is called later, from the game thread.

It is meant to be used after [`DownloadFile`](download-file.md), and it can run while the game is open: the game has not loaded a neighborhood that did not exist before. To enter the new neighborhood, restart the game (see [`RelaunchGame`](relaunch-game.md)).

Draft in Markdown, to be adapted to the format of the TS2 Extender docs site (separate repo).

## Parameters

| Name | Type | Description |
|---|---|---|
| `zipPath` | string | The `.zip` to extract. It must be a file **inside the staging folder**, `GetUserDirectory() .. "\\TS2ExtenderStaging"`. |
| `extractDir` | string | The new neighborhood folder, for example `GetUserDirectory() .. "\\Neighborhoods\\N005"`. It must be **directly inside** `<GetUserDirectory()>\Neighborhoods` (one level), and it must **not exist**. The `Neighborhoods` folder itself must exist. |
| `callback` | function | `callback(ok, err, bytes)`: `ok` is a boolean, `err` is a string or `nil`, `bytes` is the uncompressed size written (0 on failure). |

## Returns

* `true` when the extraction was started. The result comes through `callback`.
* `false, err` when it could not be started, and `callback` will **not** be called: the option is disabled, an argument is invalid, a path is not allowed, the `.zip` does not exist, `extractDir` already exists, or too many network operations are in progress.

## Contents of the zip

The contents of the `.zip` become the contents of the neighborhood folder: files such as `Neighborhood.prop` or `Lots\...` go at the root of the `.zip`, **without** an enclosing folder. The name of the folder comes only from `extractDir`, so the server cannot choose it.

## How it works

1. The whole archive is checked **before anything is written**. If any entry is unsafe, the whole `.zip` is rejected.
2. Files are extracted into a temporary folder inside the staging folder, never inside `Neighborhoods`.
3. The temporary folder is renamed to `extractDir` in a single step. The game never sees a half-extracted neighborhood.
4. On any failure the temporary folder is deleted and `Neighborhoods` is unchanged.

A `.zip` is rejected completely if it has any of:

* entry names with `..`, absolute paths, a drive letter or `:` (alternate data streams), backslashes, characters Windows does not allow, names ending in a dot or a space, or reserved names (`CON`, `NUL`, `COM1`...);
* symbolic links or other special files, encrypted entries, or compression methods other than stored/deflate;
* duplicate names (ignoring case), or a name used both as a file and as a folder;
* names that are not valid UTF-8, or paths longer than 240 characters;
* more than 50000 entries, or more than `ExtractMaxMegabytes` once uncompressed (also enforced while extracting, in case the archive lies about its sizes).

## Requirements

Disabled by default. Enable it in `TS2Extender.ini`:

```ini
[Network]
EnableNetworkApi = true
; Largest total size, once uncompressed, that will be extracted, in megabytes.
ExtractMaxMegabytes = 4096
```

## Example

```lua
local user = GetUserDirectory()
local zip = user .. "\\TS2ExtenderStaging\\hood.zip"

ExtractNeighborhood(zip, user .. "\\Neighborhoods\\N005", function(ok, err, bytes)
	if not ok then
		print("Could not add the neighborhood: " .. err)
		return
	end
	print("Neighborhood added (" .. bytes .. " bytes)")
	RelaunchGame('-load:"N005;165 Sim Lane"')
end)
```

## Notes

* If `Neighborhoods` is a symbolic link or junction (some players move it to another drive), the function refuses to work.
* Only the new network functions restrict paths. The existing `ReadFile` and `WriteFile` do not.
* The `.zip` is not deleted afterwards.
* Not tested under Wine/Linux.
