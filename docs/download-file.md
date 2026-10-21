# DownloadFile

`boolean ok, string err DownloadFile(string url, string destPath, function callback [, string expectedSha256])`

Downloads a file with an HTTP or HTTPS `GET` and saves it to `destPath`. The download runs in a background thread, so the game does not freeze: the function returns immediately and `callback` is called later, from the game thread, when the download has finished or failed.

Draft in Markdown, to be adapted to the format of the TS2 Extender docs site (separate repo).

## Parameters

| Name | Type | Description |
|---|---|---|
| `url` | string | `http://` or `https://` address, **or a path starting with `/`** (for example `/hood.zip`), which is appended to `ServerUrl` from `TS2Extender.ini`. Address (at most 2048 bytes, no `user:password@`). Redirects are followed (at most 5); a redirect from https to http is refused. HTTPS certificates are validated by Windows. |
| `destPath` | string | Where to save the file. It must be **inside the staging folder**, `GetUserDirectory() .. "\\TS2ExtenderStaging"`, which is created on first use. Sub-folders are created as needed. An existing file at `destPath` is replaced once the new download is complete and verified. |
| `callback` | function | `callback(ok, err, bytes)`: `ok` is a boolean, `err` is a string or `nil`, `bytes` is the number of bytes downloaded (0 on failure). |
| `expectedSha256` | string, optional | 64 hexadecimal characters (upper or lower case). The SHA-256 of the file is computed while it downloads; if it differs, the download fails and the file is deleted. |

## Returns

* `true` when the download was started. The result comes through `callback`.
* `false, err` when it could not be started, and `callback` will **not** be called: the option is disabled, an argument is invalid, `destPath` is not allowed, or too many network operations are already in progress (8 at most).

## Requirements

Disabled by default. Enable it in `TS2Extender.ini`:

```ini
[Network]
EnableNetworkApi = true
; Largest file accepted, in megabytes.
DownloadMaxMegabytes = 1024
; Longest a single download may take, in seconds.
DownloadTimeoutSeconds = 600
; Your server, used for urls that are just a path. Plain http is fine: localhost and VPN addresses (Radmin) work.
ServerUrl = http://26.12.34.56:3000
```

## Behaviour

* The data is written to `<destPath>.part` and renamed to `destPath` only when the whole body arrived and the hash (if given) matched. On any failure the `.part` file is deleted, and an existing `destPath` is left untouched.
* The callback receives `ok = false` and a message for: server down or unreachable, HTTP status other than 200 (for example `The server answered HTTP 404.`), certificate rejected, timeout, file larger than `DownloadMaxMegabytes`, hash mismatch, or a full disk.
* Operations run one at a time, in the order they were requested.
* If the game's Lua state is recreated while a download is running, its result is dropped and the callback is not called.
* The callback is called while the game runs a frame; if it raises an error, the error is written to the log and the game continues.

## Example

```lua
local staging = GetUserDirectory() .. "\\TS2ExtenderStaging"
local zip = staging .. "\\hood.zip"

local started, err = DownloadFile("https://example.com/hoods/N005.zip", zip, function(ok, err, bytes)
	if not ok then
		print("Download failed: " .. err)
		return
	end
	print("Downloaded " .. bytes .. " bytes")
	-- Next step: ExtractNeighborhood(zip, ...)
end, "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08")

if not started then
	print("Could not start: " .. err)
end
```

## Notes

* Only the new network functions restrict paths. The existing `ReadFile` and `WriteFile` do not.
* Symbolic links and junctions inside the staging path are refused.
* `DownloadFile` only does `GET`. For other methods, a request body or headers, see `HttpRequest` (`docs/http-request.md`).
* The staging folder is not cleaned automatically; the mod can delete the `.zip` after extracting it.
* Not tested under Wine/Linux.
