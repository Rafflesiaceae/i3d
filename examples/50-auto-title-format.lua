-- Apply a title format to each new X11 window from its WM_CLASS identity.
local title_format = {
	firefox = "🫐 %title",
	chromium = "🫐 %title",
	thunar = "📂 %title",
	urxvt = "%title",
	mpv = "📼 %title",
	["code-oss"] = "[] %title",
	code = "[] %title",
	["jetbrains-idea"] = "[] %title",
	["jetbrains-clion"] = "[] %title",
	["jetbrains-goland"] = "[] %title",
}

local function i3_quote(value)
	return value:gsub("\\", "\\\\"):gsub('"', '\\"'):gsub("[\r\n]", " ")
end

function on_window(event)
	if event.change ~= "new" or event.con_id == nil then
		return
	end
	local found = i3.find({
		con_id = event.con_id,
		fields = { "name", "window_properties" },
		limit = 1,
	})
	if #found == 0 then
		return
	end
	local node = found[1]
	local properties = node.window_properties or {}
	local candidates = { properties.class, properties.instance, node.name }
	local format = "🔩 %title"
	for _, candidate in ipairs(candidates) do
		if type(candidate) == "string" and title_format[candidate:lower()] then
			format = title_format[candidate:lower()]
			break
		end
	end
	i3.command(string.format('[con_id="%d"] title_format "%s"', event.con_id, i3_quote(format)))
end
