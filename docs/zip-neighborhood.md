# ZipNeighborhood

`boolean ok, string err ZipNeighborhood(string name, string zipDir, function callback)`

Packs a neighborhood from `Neighborhoods` into a `.zip`, for example to upload it to a server. It is the counterpart of [`ExtractNeighborhood`](extract-neighborhood.md). The neighborhood that the game has loaded is **never modified**: it is first copied to a temporary folder and the `.zip` is built from the copy. The work runs in a background thread; the function returns immediately and `callback` is called later, from the game thread.

Draft in Markdown, to be adapted to the format of the TS2 Extender docs site (separate repo).

## Parameters

| Name | Type | Description |
|---|---|---|
| `name` | string | The name of the neighborhood folder, for example `"N001"`. It is a folder name, **not a path**: it is looked up directly inside `<GetUserDirectory()>\Neighborhoods`. |
| `zipDir` | string | The folder where the `.zip` is left. It must be **inside the staging folder**, `GetUserDirectory() .. "\\TS2ExtenderStaging"` (for example `...\\TS2ExtenderStaging\\Ts2ExtenderTemp`), and it is created if it does not exist. |
| `callback` | function | `callback(ok, err, bytes)`: `ok` is a boolean, `err` is a string or `nil`, `bytes` is the size of the `.zip` that was created (0 on failure). |

## Returns

* `true` when the work was started. The result comes through `callback`.
* `false, err` when it could not be started, and `callback` will **not** be called: the option is disabled, an argument is invalid, `name` is not a plain folder name, the neighborhood does not exist, `zipDir` is not allowed, or too many network operations are in progress.

## How it works

For `ZipNeighborhood("N001", zipDir, cb)`:

1. `Neighborhoods\N001` is copied to `<zipDir>\Temp`. Files are opened for reading while sharing with the game, so a file the game has open can still be read.
2. The copy is compressed into `<zipDir>\N001.zip`. It is written as `N001.zip.part` first and renamed only when it is complete, so a half-written `.zip` is never seen.
3. `<zipDir>\Temp` is deleted, whether it worked or not.

The files of the neighborhood go at the **root** of the `.zip`, without an enclosing folder, the same layout that [`ExtractNeighborhood`](extract-neighborhood.md) expects, so a `.zip` made here can be extracted on another machine as it is. Empty folders are kept. Entry names are UTF-8.

Things to know:

* An existing `<zipDir>\N001.zip` is **replaced** by the new one. Nothing outside the staging folder is written.
* A `Temp` folder already in `zipDir` (left by a run that was interrupted) is deleted before starting. Do not keep anything of your own in a folder called `Temp` inside `zipDir`.
* Zip operations, downloads and extractions run one at a time, so two calls to `ZipNeighborhood` never share the `Temp` folder.
* The copy is a snapshot taken while the game may be running. If the game saves the neighborhood at that very moment, the copy can mix old and new files. Call it when the game is not saving (for example right after the player saved and left it idle).
* If the neighborhood contains a symbolic link or junction, or `Neighborhoods` itself is a link, the function refuses to work.
* It fails if the neighborhood has more than 50000 files and folders, or more than `ExtractMaxMegabytes` of data (counted before compressing).
* The `.zip` is not deleted afterwards, and neither is `zipDir`.

## Requirements

Disabled by default. Enable it in `TS2Extender.ini`:

```ini
[Network]
EnableNetworkApi = true
; Largest total size, before compressing, that ZipNeighborhood will pack (also the limit of ExtractNeighborhood), in megabytes.
ExtractMaxMegabytes = 4096
```

## Example

```lua
local user = GetUserDirectory()
local zipDir = user .. "\\TS2ExtenderStaging\\Ts2ExtenderTemp"

ZipNeighborhood("N001", zipDir, function(ok, err, bytes)
	if not ok then
		print("Could not zip the neighborhood: " .. err)
		return
	end
	print("Zip ready (" .. bytes .. " bytes): " .. zipDir .. "\\N001.zip")
end)
```

## Notes

* Only the new network functions restrict paths. The existing `ReadFile` and `WriteFile` do not.
* Not tested under Wine/Linux.
* To send the `.zip` to a server, use [`UploadNeighborhood`](upload-neighborhood.md).
