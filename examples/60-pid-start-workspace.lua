-- Move a new window to the workspace where its process ancestry was launched.
local pid_info = {}

local function current_workspace_name()
	for _, workspace in ipairs(i3.get_workspaces()) do
		if workspace.focused then
			return workspace.name
		end
	end
end

local function workspace_name_by_num(number)
	for _, workspace in ipairs(i3.get_workspaces()) do
		if workspace.num == number then
			return workspace.name
		end
	end
end

local function workspace_from_pid(process_id)
	local found = i3.find({ pid = process_id, fields = { "workspace_num" }, limit = 1 })
	return #found ~= 0 and workspace_name_by_num(found[1].workspace_num) or nil
end

local function on_new_pid(child_pid, parent_pid)
	local workspace = workspace_from_pid(parent_pid) or current_workspace_name()
	if workspace ~= nil then
		pid_info[child_pid] = { workspace = workspace, time = time.now_sec() }
	end
end

local function closest_tracked_ancestor(target_pid)
	local best
	for process_id, info in pairs(pid_info) do
		if time.now_sec() - info.time > 600 then
			pid_info[process_id] = nil
		elseif pid.is_ancestor(process_id, target_pid) and (best == nil or pid.is_ancestor(best, process_id)) then
			best = process_id
		end
	end
	return best
end

function on_window(event)
	if event.change ~= "new" or event.con_id == nil then
		return
	end
	local window_pid = i3.get_window_pid(event.con_id)
	local root_pid = window_pid and closest_tracked_ancestor(window_pid)
	if root_pid == nil then
		return
	end
	local workspace = pid_info[root_pid].workspace:gsub("\\", "\\\\"):gsub('"', '\\"')
	i3.command(string.format('[con_id="%d"] move container to workspace "%s"', event.con_id, workspace))
end

-- The process connector is preferred; i3d falls back to /proc polling.
pid_watch_stop = pid.watch_new(on_new_pid)
