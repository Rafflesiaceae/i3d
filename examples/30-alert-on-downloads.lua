-- Notify at low urgency when a file is created or moved into Downloads.
local download_directory = "~/Downloads"

local function on_download_added(path)
	-- Chromium first creates a temporary file, then moves the completed
	-- download to its final name. The move event reports that final file.
	if path:match("%.crdownload$") then
		return
	end
	if path:match(".org.chromium.Chromium%") then
		return
	end

	local name = path:match("([^/]+)$") or path
	exec({ "notify-send", "--urgency=low", "Downloaded: '"..name.."'", ""}, false, false, false)
end

function init()
	download_watch_stop = inotify.watch_new(download_directory, on_download_added)
end
