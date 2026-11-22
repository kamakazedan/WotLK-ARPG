--[[
    Camera.lua: the CVars the mode needs, and the camera itself.

    With the client library the camera is locked to a set direction, angle
    and distance (ARPG_SetCamera), and nothing the mouse or the game does can
    move it; indoors the library shows the building from inside
    (ARPG_SetCutaway, see Core.lua). Without the library the camera can only
    be "parked": it stops following the character, its zoom keys are disabled
    and a saved view is restored, but dragging the mouse still turns it.

    The CVars are the mode's only for as long as a session lasts. The client
    saves most of them per account, and hands the account's saved settings to
    the server, which hands them to whichever client logs in next. So the
    player's own values go back before the client saves anything (see
    PLAYER_LOGOUT in Core.lua), and the mode's are applied again at login.
]]

local ADDON, ns = ...

ns.VIEW_SLOT = 5 -- which of the client's saved camera views holds the ARPG camera

-- CVar values while the mode is on. The player's own are kept in ns.ownCVars
-- meanwhile and put back when the mode goes off and whenever the UI shuts
-- down. An entry with a third field is only applied while that setting is on.
-- All but the last are saved per account (the client registers them with
-- flag 0x10) and so reach the server; the last is saved per machine.
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

-- Up to 0.3.0 the player's own values were snapshotted once, in the saved
-- variables, and the mode's values stayed in the CVars across logouts: so
-- they were saved, sent to the server, and turned up on every other client
-- the account logged in from. `legacy` is such a snapshot, met at the first
-- login after that. Where a CVar still holds the mode's value, the snapshot
-- has the player's own; where it holds anything else, the player has set it
-- since, and that stands.
function ns:AdoptSavedCVars(legacy)
    local own = {}
    for _, entry in ipairs(CVARS) do
        local name, wanted = entry[1], entry[2]
        if legacy[name] ~= nil and getCVar(name) == wanted then
            own[name] = legacy[name]
        end
    end
    self.ownCVars = own
end

function ns:ApplyCVars()
    local db = self.db
    self.ownCVars = self.ownCVars or {}
    local own = self.ownCVars
    for _, entry in ipairs(CVARS) do
        local name, wanted, setting = entry[1], entry[2], entry[3]
        local current = getCVar(name)
        if current == nil then
            -- this client has no such CVar
        elseif setting and not db[setting] then
            -- Switched off in the settings: give the player's own value back.
            if own[name] ~= nil then
                setCVar(name, own[name])
                own[name] = nil
            end
        else
            -- Note the player's own value once, before the first change.
            if own[name] == nil then
                own[name] = current
            end
            if current ~= wanted then
                setCVar(name, wanted)
            end
        end
    end
end

-- Puts the player's own values back. With the mode still on (the UI is
-- shutting down), the next login applies the mode's again.
function ns:RestoreCVars()
    local own = self.ownCVars
    if not own then return end
    for name, value in pairs(own) do
        setCVar(name, value)
    end
    self.ownCVars = nil
end

---------------------------------------------------------------------------
-- Locked camera (client library)
---------------------------------------------------------------------------

-- An indoor camera that ignores walls sits above the roof looking at tiles.
-- The inside view deals with that. Where there is none (switched off, or an
-- older library), the camera may collide indoors instead, which keeps the
-- character in view by moving in.
function ns:WantsCollision()
    return (self.db.indoorCollision and IsIndoors() and not self:CutawayWorks()) and true or false
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
    if not self.char.hasView then
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
    self.char.hasView = true
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
