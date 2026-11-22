--[[
    Input.lua: the mouse and key layer.

    Every change is an override binding owned by one secure handler frame, so
    the player's saved bindings are never modified. The handler can rebuild
    its bindings from inside combat, which is what lets the WASD toggle key
    work mid-fight; everything else waits for combat to end (see ns:Sync).
]]

local ADDON, ns = ...

-- Keys bound to these commands do nothing while movement is locked.
local MOVE_COMMANDS = {
    "MOVEFORWARD", "MOVEBACKWARD", "TURNLEFT", "TURNRIGHT",
    "STRAFELEFT", "STRAFERIGHT", "TOGGLEAUTORUN", "MOVEANDSTEER",
}

-- Keys bound to these commands do nothing while the mode is on, so only the
-- camera settings can move the camera. By default this covers the scroll wheel.
local CAMERA_COMMANDS = {
    "CAMERAZOOMIN", "CAMERAZOOMOUT", "NEXTVIEW", "PREVVIEW", "FLIPCAMERAYAW",
    "SETVIEW1", "SETVIEW2", "SETVIEW3", "SETVIEW4", "SETVIEW5",
    "SAVEVIEW1", "SAVEVIEW2", "SAVEVIEW3", "SAVEVIEW4", "SAVEVIEW5",
}

-- Disabled keys are pointed at this button, which has no click handler.
CreateFrame("Button", "WotLKARPGNoop", UIParent)

local handler = CreateFrame("Button", "WotLKARPGMoveLock", UIParent, "SecureHandlerClickTemplate")
handler:RegisterForClicks("AnyDown")

-- Runs in the restricted environment. The client rejects snippets containing
-- curly braces or the Lua keyword for defining a closure, so keep both out.
local REBUILD = [[
    self:ClearBindings()
    if not self:GetAttribute("arpg-on") then
        return
    end
    if self:GetAttribute("arpg-movelock") then
        for i = 1, self:GetAttribute("arpg-nmove") or 0 do
            self:SetBindingClick(true, self:GetAttribute("arpg-move" .. i), "WotLKARPGNoop")
        end
    end
    if not self:GetAttribute("arpg-camedit") then
        for i = 1, self:GetAttribute("arpg-ncam") or 0 do
            self:SetBindingClick(true, self:GetAttribute("arpg-cam" .. i), "WotLKARPGNoop")
        end
        self:SetBinding(true, "BUTTON1", self:GetAttribute("arpg-left"))
        self:SetBindingClick(true, "BUTTON2", "WotLKARPGNoop")
    end
]]

-- Pressing the "Toggle WASD movement" key clicks the handler. A click from a
-- key press is a secure action, so this runs in combat too.
handler:SetAttribute("_onclick", [[
    if not self:GetAttribute("arpg-on") then
        return
    end
    self:SetAttribute("arpg-movelock", not self:GetAttribute("arpg-movelock"))
]] .. REBUILD)

-- Keep the saved setting in step when the key toggles the lock.
handler:SetScript("OnAttributeChanged", function(_, name, value)
    if name ~= "arpg-movelock" or not ns.db then return end
    value = value and true or false
    if ns.db.moveLocked ~= value then
        ns.db.moveLocked = value
        ns.Print(value and "WASD locked." or "WASD unlocked.")
    end
end)

-- All keys currently bound to any of the commands, without duplicates.
local function collectKeys(commands)
    local keys, seen = {}, {}
    for _, command in ipairs(commands) do
        local bound = { GetBindingKey(command) }
        for _, key in ipairs(bound) do
            if not seen[key] then
                seen[key] = true
                keys[#keys + 1] = key
            end
        end
    end
    sort(keys)
    return keys
end

local function signature(moveKeys, cameraKeys)
    return table.concat(moveKeys, " ") .. "|" .. table.concat(cameraKeys, " ")
end

local function setKeyList(name, keys)
    handler:SetAttribute("arpg-n" .. name, #keys)
    for i, key in ipairs(keys) do
        handler:SetAttribute("arpg-" .. name .. i, key)
    end
end

-- What the left mouse button does in the world. TURNORACTION is the client's
-- own click-to-move action: one click, one move. The client library replaces
-- it with hold-to-move.
function ns:LeftButtonCommand()
    return "TURNORACTION"
end

-- True when the player has rebound a movement or camera key since the last
-- rebuild. Override bindings do not show up in GetBindingKey, so rebuilding
-- never triggers itself through UPDATE_BINDINGS.
function ns:InputChanged()
    return signature(collectKeys(MOVE_COMMANDS), collectKeys(CAMERA_COMMANDS)) ~= self.inputSignature
end

-- Pushes the settings into the secure handler and rebuilds the overrides.
-- Protected frames cannot be changed in combat: only call this from ns:Sync.
function ns:RefreshInput()
    local db = self.db
    local moveKeys, cameraKeys = collectKeys(MOVE_COMMANDS), collectKeys(CAMERA_COMMANDS)
    self.inputSignature = signature(moveKeys, cameraKeys)

    handler:SetAttribute("arpg-on", db.enabled and true or false)
    handler:SetAttribute("arpg-camedit", self.cameraEdit and true or false)
    handler:SetAttribute("arpg-movelock", db.moveLocked and true or false)
    handler:SetAttribute("arpg-left", self:LeftButtonCommand())
    setKeyList("move", moveKeys)
    setKeyList("cam", cameraKeys)
    handler:Execute(REBUILD)
end

function ns:ToggleMoveLock()
    if not self.db.enabled then
        ns.Print("WASD is only locked while the mode is on (/arpg).")
        return
    end
    if InCombatLockdown() then
        ns.Print("In combat, use the key under Key Bindings > WotLK-ARPG instead.")
        return
    end
    self.db.moveLocked = not self.db.moveLocked
    self:RefreshInput()
    ns.Print(self.db.moveLocked and "WASD locked." or "WASD unlocked.")
end

-- Undo for anyone who rebound the mouse buttons by hand while testing.
function ns:ResetMouseBindings()
    if InCombatLockdown() then
        ns.Print("Key bindings cannot change in combat. Try again afterwards.")
        return
    end
    SetBinding("BUTTON1", "CAMERAORSELECTORMOVE")
    SetBinding("BUTTON2", "TURNORACTION")
    SaveBindings(GetCurrentBindingSet())
    ns.Print("Left and right mouse buttons are back to the defaults.")
end
