--[[
    Camera.lua: the CVars the mode needs, and the camera itself.

    With the client library the camera is locked to a set direction, angle
    and distance (ARPG_SetCamera), and nothing the mouse or the game does can
    move it. Without the library the camera can only be "parked": it stops
    following the character, its zoom keys are disabled and a saved view is
    restored, but dragging the mouse still turns it.
]]

local ADDON, ns = ...

ns.VIEW_SLOT = 5 -- which of the client's saved camera views holds the ARPG camera

-- CVar values while the mode is on. The originals are snapshotted in the
-- saved variables and put back when the mode is turned off. An entry with a
-- third field is only applied while that setting is on.
local CVARS = {
    { "autointeract", "1" },              -- click-to-move (hold-to-move builds on it)
    { "cameraSmoothStyle", "0" },         -- camera following style: never
    { "cameraSmoothTrackingStyle", "0" }, -- click-to-move camera style: never
    { "cameraDistanceMax", "50" },        -- room for a high top-down camera
    { "cameraTerrainTilt", "0" },         -- do not tilt with slopes
    { "cameraViewBlendStyle", "2" },      -- saved views snap instead of gliding
    -- "Sound at Character" off: the listener sits at the camera and faces the
    -- way it faces, so a sound on the right of the screen is heard on the right.
    { "Sound_ListenerAtCharacter", "0", "soundAtCamera" },
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
        local name, wanted, setting = entry[1], entry[2], entry[3]
        local current = getCVar(name)
        if current == nil then
            -- this client has no such CVar
        elseif setting and not db[setting] then
            -- Switched off in the settings: give the player's own value back.
            if db.savedCVars[name] ~= nil then
                setCVar(name, db.savedCVars[name])
                db.savedCVars[name] = nil
            end
        else
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

---------------------------------------------------------------------------
-- Locked camera (client library)
---------------------------------------------------------------------------

-- Until buildings can be cut away, an indoor camera that ignores walls would
-- sit above the roof looking at tiles. Indoors it may collide instead.
function ns:WantsCollision()
    return (self.db.indoorCollision and IsIndoors()) and true or false
end

-- Locks or releases the camera to match the settings. Safe to call at any
-- time, including in combat.
function ns:ApplyClientCamera()
    if not self:ClientCanLockCamera() then
        self.cameraLocked = nil
        return
    end
    local db = self.db
    if db.enabled and not self.cameraEdit then
        self.cameraCollides = self:WantsCollision()
        self.cameraLocked = true
        ARPG_SetCamera(db.camYaw, db.camPitch, db.camDistance, self.cameraCollides)
    else
        self.cameraLocked = nil
        ARPG_SetCamera() -- release
    end
end

---------------------------------------------------------------------------
-- Saved view (no client library)
---------------------------------------------------------------------------

function ns:ApplyCamera(verbose)
    if self:ClientCanLockCamera() then
        if verbose then
            ns.Print("The camera is locked by the client library. Change it with /arpg options.")
        end
        return
    end
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
    if self:ClientCanLockCamera() then
        ns.Print("With the client library the camera is set in /arpg options, not saved from a view.")
        return
    end
    SaveView(self.VIEW_SLOT)
    self.db.hasView = true
    ns.Print("Camera saved. /arpg cam snaps back to it at any time.")
    if self.cameraEdit then
        self:SetCameraEdit(false)
    end
end

-- Camera edit hands the camera, the mouse buttons and the scroll wheel back
-- to the client: to look around, or to set up a view to save.
function ns:SetCameraEdit(on)
    if on and not self.db.enabled then
        ns.Print("Turn the mode on first (/arpg).")
        return
    end
    self.cameraEdit = on or nil
    if not self:Sync() then
        return -- in combat: finishes afterwards
    end
    if not on then
        ns.Print("Camera locked.")
    elseif self:ClientCanLockCamera() then
        ns.Print("Camera free: scroll to zoom, hold the left mouse button to look around. /arpg camedit again to lock it.")
    else
        ns.Print("Camera edit: scroll to zoom, hold the left mouse button to turn and tilt, then /arpg savecam.")
    end
end
