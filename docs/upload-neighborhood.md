# UploadNeighborhood

`boolean ok, string err UploadNeighborhood(string url, string zipPath, function callback)`

Uploads a `.zip` (for example one made with [`ZipNeighborhood`](zip-neighborhood.md)) to a server with an HTTP `POST`. The file is sent as the raw body of the request, streamed from disk in chunks, so a big neighborhood is never loaded into memory. The work runs in a background thread; the function returns immediately and `callback` is called later, from the game thread.

Draft in Markdown, to be adapted to the format of the TS2 Extender docs site (separate repo).

## Parameters

| Name | Type | Description |
|---|---|---|
| `url` | string | A full `http://` or `https://` URL, **or** a path that starts with `/` (for example `"/upload/N001"`), which is appended to `ServerUrl` from `TS2Extender.ini`. |
| `zipPath` | string | The `.zip` to send. It must be a file ending in `.zip` **inside the staging folder**, `GetUserDirectory() .. "\\TS2ExtenderStaging"`. |
| `callback` | function | `callback(ok, err, status, response)`: the same as in [`HttpRequest`](http-request.md). `ok` is `true` only for a 2xx status; `err` is a string or `nil`; `status` is the HTTP status (0 if the server never answered); `response` is the body of the answer (up to 1 MB), also when `ok` is `false` but the server did answer. |

## Returns

* `true` when the upload was started. The result comes through `callback`.
* `false, err` when it could not be started, and `callback` will **not** be called: the option is disabled, an argument is invalid, `url` is a path but `ServerUrl` is empty, `zipPath` is outside the staging folder or is not a `.zip`, the file does not exist, is empty or is bigger than `UploadMaxMegabytes`, or too many network operations are in progress.

## What the server receives

```
POST <url> HTTP/1.1
Content-Type: application/zip
Content-Length: <size of the file>

<the bytes of the .zip>
```

Nothing else is added: no multipart, no file name. Put what the server needs to know (the neighborhood name, the player...) in the `url`, for example `"/upload/N001"`. The body is the same layout [`ExtractNeighborhood`](extract-neighborhood.md) expects, so the server can hand it to another player as it is.

Things to know:

* **Redirects are not followed.** A redirected `POST` would be sent again as a `GET` without the file; a 3xx answer comes back as `ok = false` with its status.
* The upload is sent in a **single** request, in the same worker thread as downloads, extractions and zips. They run one at a time and in the order requested, so `ZipNeighborhood` followed by `UploadNeighborhood` never uploads a half-made `.zip`; you can also chain them from the callback, as in the example.
* The file is opened while the upload lasts so nobody can write to it; if it changes in spite of that, the upload fails.
* The total time is limited by `DownloadTimeoutSeconds` (600 by default), and a single send or receive step that makes no progress for 30 seconds fails.
* There is no authentication, no retry and no hash: the mod decides (it can put a token in the `url`, or use [`HttpRequest`](http-request.md) for a request before or after). With plain `http` the file travels unencrypted.
* Only `.zip` files from the staging folder can be sent through this function; it is not a general file uploader.

## Requirements

Disabled by default. Enable it in `TS2Extender.ini`:

```ini
[Network]
EnableNetworkApi = true
; Address of your server (needed for urls that are just a path).
ServerUrl = http://127.0.0.1:3000
; Largest .zip UploadNeighborhood will send, in megabytes (at most 4095).
UploadMaxMegabytes = 1024
; Longest a single upload may take, in seconds (shared with DownloadFile).
DownloadTimeoutSeconds = 600
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
	UploadNeighborhood("/upload/N001", zipDir .. "\\N001.zip", function(ok2, err2, status, response)
		if not ok2 then
			print("Upload failed (HTTP " .. status .. "): " .. err2 .. " " .. response)
			return
		end
		print("Uploaded. The server said: " .. response)
	end)
end)
```

Server side (Node, only to show the shape of the request; the body arrives as the raw file):

```js
app.post('/upload/:name', express.raw({ type: 'application/zip', limit: '1gb' }), (req, res) => {
	fs.writeFileSync(`${req.params.name}.zip`, req.body)
	res.status(201).send('ok')
})
```

## Notes

* Only the new network functions restrict paths. The existing `ReadFile` and `WriteFile` do not.
* Tested in the console against a local server (see `tests/`), not yet in the game and not over Radmin.
* Not tested under Wine/Linux.
