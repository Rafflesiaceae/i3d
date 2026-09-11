-- Hide matching windows in the i3 scratchpad under a unique mark.
-- Entries accept exactly one of plain/pattern and may add class_plain or
-- class_pattern. ignore_case applies to both title and class matching.
local title_matches = {
	-- {plain = "Authentication Required"},
	-- {pattern = "^update available.*$", ignore_case = true,
	--  class_plain = "Firefox"},
	-- {plain = "Sign in", class_pattern = "^chromium$", ignore_case = true},
}

local function matches(value, plain, pattern, ignore_case)
	if value == nil then
		return false
	end
	if ignore_case then
		value = value:lower()
		plain = plain and plain:lower()
		pattern = pattern and pattern:lower()
	end
	if plain ~= nil then
		return value:find(plain, 1, true) ~= nil
	end
	if pattern ~= nil then
		local valid, result = pcall(string.find, value, pattern)
		if not valid then
			log("hide-windows-by-title: invalid Lua pattern: " .. pattern)
			return false
		end
		return result ~= nil
	end
	return false
end

local function entry_matches(entry, title, class_name)
	if (entry.plain == nil) == (entry.pattern == nil) then
		log("hide-windows-by-title: set exactly one of plain or pattern")
		return false
	end
	if entry.class_plain ~= nil and entry.class_pattern ~= nil then
		log("hide-windows-by-title: set at most one class matcher")
		return false
	end
	return matches(title, entry.plain, entry.pattern, entry.ignore_case)
		and (
			entry.class_plain == nil and entry.class_pattern == nil
			or matches(class_name, entry.class_plain, entry.class_pattern, entry.ignore_case)
		)
end

function on_window(event)
	if (event.change ~= "new" and event.change ~= "title") or event.con_id == nil then
		return
	end
	local mark = string.format("i3d-hidden-%d", event.con_id)
	for _, existing in ipairs(i3.get_marks()) do
		if existing == mark then
			return
		end
	end
	local found = i3.find({ con_id = event.con_id, fields = { "name", "window_properties" }, limit = 1 })
	if #found == 0 then
		return
	end
	local node = found[1]
	local class_name = (node.window_properties or {}).class
	for _, entry in ipairs(title_matches) do
		if entry_matches(entry, node.name, class_name) then
			if i3.command(string.format('[con_id="%d"] mark --add "%s"', event.con_id, mark)) then
				-- The generated mark contains only a fixed prefix and digits;
				-- anchors make i3's own criteria match exact.
				i3.command(string.format('[con_mark="^%s$"] move scratchpad', mark))
			end
			return
		end
	end
end
