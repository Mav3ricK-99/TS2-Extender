# HttpRequest

`boolean ok, string err HttpRequest(string method, string url, string body, string headers, function callback)`

Sends an HTTP or HTTPS request and gives you the answer. The request runs in a background thread, so the game does not freeze: the function returns immediately and `callback` is called later, from the game thread, when the server answered or the request failed.

Draft in Markdown, to be adapted to the format of the TS2 Extender docs site (separate repo).

The game's Lua has no JSON library, so if you send JSON, **your mod builds the text** and `HttpRequest` only sends it. A small builder is shown at the end of this page.

## Parameters

| Name | Type | Description |
|---|---|---|
| `method` | string | `"GET"`, `"HEAD"`, `"POST"`, `"PUT"`, `"PATCH"`, `"DELETE"` or `"OPTIONS"`, in upper case. |
| `url` | string | `http://` or `https://` address (at most 2048 bytes, no `user:password@`), **or a path starting with `/`** (for example `/events`), which is appended to `ServerUrl` from `TS2Extender.ini`. HTTPS certificates are validated by Windows. |
| `body` | string or `nil` | The request body, at most 1 MB. Sent as it is (UTF-8 if you give it UTF-8). `GET` and `HEAD` cannot have a body. `nil` or `""` sends none. |
| `headers` | string or `nil` | Extra headers, one `Name: value` per line, separated by `"\n"`. For example `"Content-Type: application/json\nAuthorization: Bearer abc"`. At most 32 headers and 8 KB. Values must be plain ASCII. `Host`, `Content-Length`, `Transfer-Encoding`, `Connection`, `Upgrade`, `Expect`, `TE` and `Trailer` cannot be set. |
| `callback` | function | `callback(ok, err, status, response)`, see below. |

The game's Lua cannot tell how many arguments you passed, so **always pass all five**, using `nil` for a `body` or `headers` you do not need.

## Callback

| Argument | Type | Description |
|---|---|---|
| `ok` | boolean | `true` when the server answered with a 2xx status. |
| `err` | string or `nil` | `nil` on success. Otherwise a message, for example `The server answered HTTP 400.` or `Could not connect to the server.` |
| `status` | number | The HTTP status code, or `0` if the server never answered (server down, timeout...). |
| `response` | string | The body of the answer (at most 1 MB), empty if there was none. It is also filled when `ok` is `false` but the server did answer, so you can read an error such as `{"error":"..."}`. |

## Returns

* `true` when the request was queued. The result comes through `callback`.
* `false, err` when it could not be queued, and `callback` will **not** be called: the option is disabled, an argument is invalid, `url` is a path but `ServerUrl` is empty, or too many requests are already waiting (32 at most).

## Requirements

Disabled by default. Enable it in `TS2Extender.ini`:

```ini
[Network]
EnableNetworkApi = true
; Your server, used for urls that are just a path. Plain http is fine: localhost and VPN addresses (Radmin) work.
ServerUrl = http://26.12.34.56:3000
; Longest a single request may take, in seconds (default 15).
HttpTimeoutSeconds = 15
```

## Behaviour

* Requests have their own background thread, apart from `DownloadFile` and `ExtractNeighborhood`: a long download does not delay them. They are sent one at a time, **in the order they were requested**, and their callbacks arrive in that same order.
* Redirects are followed (at most 5) only for `GET` and `HEAD`. For the other methods they are **not** followed, because a redirected `POST` would silently become a `GET` without the body; a `301`/`302` answer comes back as `ok = false` with that `status`.
* There is no automatic retry. If the server is down, the callback gets `ok = false` and `status = 0`; deciding whether to try again is up to the mod.
* A hung server is cut by WinHTTP's timeouts, roughly within twice `HttpTimeoutSeconds`.
* If the game's Lua state is recreated while a request is running, its result is dropped and the callback is not called.
* The callback is called while the game runs a frame; if it raises an error, the error is written to the log and the game continues.
* The answer is kept in memory and limited to 1 MB. To save a big file to disk, use `DownloadFile`.

## Examples

```lua
-- GET
HttpRequest("GET", "/status", nil, nil, function(ok, err, status, response)
	if ok then print("Server says: " .. response) end
end)

-- POST with JSON
local json = '{"type":"hello","seq":1}'
HttpRequest("POST", "/events", json, "Content-Type: application/json", function(ok, err, status, response)
	if not ok then
		print("Request failed: " .. err .. " (status " .. status .. ") " .. response)
		return
	end
	print("Server said: " .. response)
end)

-- DELETE with a custom header, without body
HttpRequest("DELETE", "/events/42", nil, "Authorization: Bearer abc", function(ok, err, status, response) end)
```

## Building the JSON in Lua

Lua 5.0 (the game's version) has no `#`, no `...` and no `string.gmatch`; this builder avoids them. It is the one the multiplayer mod uses.

```lua
local function jstr(s)
	s = tostring(s)
	s = string.gsub(s, "\\", "\\\\")
	s = string.gsub(s, "\"", "\\\"")
	s = string.gsub(s, "\n", "\\n")
	s = string.gsub(s, "\r", "\\r")
	s = string.gsub(s, "\t", "\\t")
	return "\"" .. s .. "\""
end

local function jval(v)
	local t = type(v)
	if t == "nil" then return "null" end
	if t == "boolean" then
		if v then return "true" end
		return "false"
	end
	if t == "number" then
		if v ~= v then return "null" end
		return tostring(v)
	end
	if t == "string" then return jstr(v) end
	return jstr("<" .. t .. ">")
end

-- fields: list of { key, already_encoded_json }
local function jobj(fields)
	local s = "{"
	for i = 1, table.getn(fields) do
		if i > 1 then s = s .. "," end
		s = s .. jstr(fields[i][1]) .. ":" .. fields[i][2]
	end
	return s .. "}"
end

local body = jobj({
	{ "seq", jval(1) },
	{ "owner", jstr("Bella Goth") },
	{ "obj", jobj({ { "ref", jstr("0x1234@10,12,0") } }) },
})
HttpRequest("POST", "/events", body, "Content-Type: application/json", function(ok, err, status, response) end)
```

Control characters other than `\n`, `\r` and `\t` are not escaped by this builder (JSON requires it), so strip them from text that comes from the game if a strict server parser complains.

## Notes

* Only the new network functions restrict what they do. The existing `ReadFile` and `WriteFile` do not.
* `url` may point to any host, as in `DownloadFile`: a mod that can call `HttpRequest` can send data to any server. That is why the whole network API is off until the player enables `EnableNetworkApi`.
* Plain `http` is unencrypted. Use `https` if the data crosses the internet; on a VPN such as Radmin, plain `http` is the usual choice. There is no built-in authentication: pass your own header if the server needs one.
* No `Content-Type` is added by default: set it in `headers` when you send a body.
* Not tested under Wine/Linux.
