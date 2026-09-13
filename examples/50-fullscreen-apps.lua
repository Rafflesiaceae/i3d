-- Move fullscreen windows to the first free workspace starting at four.
-- Match lowercased X11 instance and class names for apps that manage fullscreen
-- transitions themselves and should stay on their current workspace.
local ignored_class_names = {
	chromium = true,
	firefox = true,
	mpv = true,
}
local base_workspace = 4
local scan_max = 64

local function has_other_fullscreen(workspace, excluded_id)
	local found = i3.find({
		where = { type = "con", workspace_num = workspace, fullscreen = true },
		fields = { "con_id" },
		limit = 2,
	})
	for _, node in ipairs(found) do
		if node.con_id ~= nil and node.con_id ~= excluded_id then
			return true
		end
	end
	return false
end

local function is_ignored(con_id)
	local names = i3.get_class_names(con_id) or {}
	for _, name in ipairs(names) do
		if type(name) == "string" and ignored_class_names[name:lower()] then
			return true
		end
	end
	return false
end

function on_window(event)
	if
		event.change ~= "fullscreen_mode"
		or event.fullscreen_mode == nil
		or event.fullscreen_mode == 0
		or event.con_id == nil
		or is_ignored(event.con_id)
	then
		return
	end
	local target = base_workspace
	for candidate = base_workspace, base_workspace + scan_max - 1 do
		if not has_other_fullscreen(candidate, event.con_id) then
			target = candidate
			break
		end
	end
	if event.workspace_num == target then
		return
	end
	i3.command(string.format('[con_id="%d"] focus', event.con_id))
	i3.command(string.format("move container to workspace number %d", target))
	i3.command(string.format("workspace number %d", target))
end
