--[[
    Camera.lua: the CVars the mode needs, and the saved camera view.

    Without the client library the camera is only "parked": it stops following
    the character and its zoom is locked, but the stock client cannot pin its
    compass direction or stop it colliding with walls. Those come from the
    client library (see docs/DESIGN.md).
]]

local ADDON, ns = ...

ns.VIEW_SLOT = 5 -- which of the client's saved camera views holds the ARPG camera

-- CVar values while the mode is on. The originals are snapshotted in the
-- saved variables and put back when the mode is turned off.
local CVARS = {
    { "autointeract", "1" },              -- click-to-move
    { "cameraSmoothStyle", "0" },         -- camera following style: never
    { "cameraSmoothTrackingStyle", "0" }, -- click-to-move camera style: never
    { "cameraDistanceMax", "50" },        -- room for a high top-down camera
    { "cameraTerrainTilt", "0" },         -- do not tilt with slopes
    { "cameraViewBlendStyle", "2" },      -- saved views snap instead of gliding
}

-- CVars differ between client builds, so never assume one exists.
local function getCVar(name)
    local ok, value = pcall(GetCVar, name)
    if ok then
        return value
    end
end

local function setCVar(name, value)
    if getCVar(name) == nil then
        return false
    end
    return (pcall(SetCVar, name, value))
end

function ns:ApplyCVars()
    local db = self.db
    db.savedCVars = db.savedCVars or {}
    for _, entry in ipairs(CVARS) do
        local name, wanted = entry[1], entry[2]
        local current = getCVar(name)
        if current ~= nil then
            -- Snapshot once, so a later login cannot overwrite the player's
            -- real setting with the value the mode itself wrote.
            if db.savedCVars[name] == nil then
                db.savedCVars[name] = current
            end
            if current ~= wanted then
                setCVar(name, wanted)
            end
        end
    end
end

function ns:RestoreCVars()
    local saved = self.db.savedCVars
    if not saved then return end
    for name, value in pairs(saved) do
        setCVar(name, value)
    end
    self.db.savedCVars = nil
end

function ns:ApplyCamera(verbose)
    if not self.db.hasView then
        if verbose then
            ns.Print("No camera saved yet: /arpg camedit, set it up, then /arpg savecam.")
        end
        return
    end
    SetView(self.VIEW_SLOT)
    SetView(self.VIEW_SLOT) -- asking twice makes the client snap instead of glide
end

function ns:SaveCamera()
    SaveView(self.VIEW_SLOT)
    self.db.hasView = true
    ns.Print("Camera saved. /arpg cam snaps back to it at any time.")
    if self.cameraEdit then
        self:SetCameraEdit(false)
    end
end

-- Camera edit hands the mouse buttons and the scroll wheel back to the
-- client, so the camera can be zoomed and turned into place.
function ns:SetCameraEdit(on)
    if on and not self.db.enabled then
        ns.Print("Turn the mode on first (/arpg).")
        return
    end
    self.cameraEdit = on or nil
    if not self:Sync() then
        return -- in combat: applies afterwards
    end
    if on then
        ns.Print("Camera edit: scroll to zoom, hold the left mouse button to turn and tilt, then /arpg savecam.")
    else
        ns.Print("Camera locked.")
    end
end
