--[[
    WotLK-ARPG - Diablo-style camera and controls for the 3.3.5a client.

    Core.lua: saved variables, the mode switch, events and the /arpg command.
    Camera.lua, Input.lua and Options.lua add the pieces the switch drives.

    Everything the mode changes is reversible: CVars are snapshotted and put
    back, and every key or mouse change is an override binding that vanishes
    when the mode is turned off. The saved key bindings are never touched.

    The addon works alone on a stock client. With the client library
    (WotLK-ARPG.dll, see client/ in the repository) it also gets hold-to-move,
    a camera locked to a set direction, angle and distance, and buildings
    with their roofs off while the character is inside.

    Nothing here touches or needs any other addon.
]]

local ADDON, ns = ...

WotLKARPG = ns -- handy for /run while testing
ns.version = GetAddOnMetadata(ADDON, "Version") or "dev"

BINDING_HEADER_WOTLKARPG = "WotLK-ARPG"
BINDING_NAME_WOTLKARPG_TOGGLE = "Toggle ARPG mode"
BINDING_NAME_WOTLKARPG_HOLDMOVE = "Move and interact (hold to keep moving)"
_G["BINDING_NAME_CLICK WotLKARPGMoveLock:LeftButton"] = "Toggle WASD movement"

-- Settings, shared by every character on the account (WotLKARPGDB). The mode
-- is on from the first login: nobody installs this to play the usual way.
local defaults = {
    enabled = true,         -- ARPG mode is on
    moveLocked = true,      -- movement keys are disabled while the mode is on
    camYaw = 0,             -- degrees; 0 looks north, so north is up on screen
    camPitch = 60,          -- degrees above the horizon the camera looks down from
    camDistance = 22,       -- yards from the character
    cutaway = true,         -- indoors, leave out whatever is above the character's head
    cutawayHeight = 4,      -- yards above the character's feet at which the world is cut indoors
    cutawayOutdoors = true, -- elsewhere, also cut a building that stands between the camera and the character
    indoorCollision = true, -- without the cutaway: indoors, let the camera move in when a wall is in the way
    groundClicks = true,    -- move clicks on roofs and treetops go to the ground under the cursor
    groundClickHeight = 3,  -- yards above the character's feet that still count as somewhere to walk
    soundAtCamera = true,   -- hear the world from the camera, so left and right match the screen
}
ns.defaults = defaults

-- Kept per character (WotLKARPGCharDB), because the client keeps the things
-- they describe per character: the CVars to put back when the mode goes off
-- (savedCVars), and whether the camera view slot holds the ARPG camera.
local characterDefaults = {
    hasView = false,
}

function ns.Print(msg)
    DEFAULT_CHAT_FRAME:AddMessage("|cffc41f3bARPG|r " .. tostring(msg))
end

---------------------------------------------------------------------------
-- Client library
---------------------------------------------------------------------------

-- Feature bits reported by ARPG_ClientVersion(), one per working hook.
ns.FEATURE_HOLD_MOVE = 2
ns.FEATURE_CAMERA_ANGLES = 4
ns.FEATURE_CAMERA_DISTANCE = 8
ns.FEATURE_CUTAWAY = 16

function ns:HasClient()
    return type(ARPG_ClientVersion) == "function"
end

function ns:ClientHas(feature)
    if not self:HasClient() then
        return false
    end
    local _, features = ARPG_ClientVersion()
    return bit.band(features or 0, feature) ~= 0
end

function ns:ClientCanHoldMove()
    return self:ClientHas(self.FEATURE_HOLD_MOVE)
end

function ns:ClientCanLockCamera()
    return self:ClientHas(self.FEATURE_CAMERA_ANGLES) and self:ClientHas(self.FEATURE_CAMERA_DISTANCE)
end

-- Bound to the left mouse button while the mode is on (see Input.lua). The
-- press and the release only tell the library; it does the moving.
function WotLKARPG_HoldMove(keystate)
    if not ns:ClientCanHoldMove() then
        return
    end
    if keystate == "down" then
        ARPG_HoldStart()
    else
        ARPG_HoldStop()
    end
end

-- Whenever the controls are switched, forget a button that was held down at
-- the time: its release may go to a different binding and never get here.
function ns:StopHold()
    if self:ClientCanHoldMove() then
        ARPG_HoldStop()
    end
end

-- Roofs and treetops: tell the library whether, and from what height, a move
-- click should go to the ground under the cursor instead.
function ns:ClientCanGroundClick()
    return type(ARPG_SetGroundClicks) == "function"
end

function ns:ApplyGroundClicks()
    if not self:ClientCanGroundClick() then
        return
    end
    if self.db.enabled and self.db.groundClicks then
        ARPG_SetGroundClicks(self.db.groundClickHeight)
    else
        ARPG_SetGroundClicks()
    end
end

-- The cutaway: indoors, the library leaves out everything above the
-- character's head and draws the rooms below. The library decides frame by
-- frame whether there is anything to cut; this only says whether it may.
function ns:ClientCanCutAway()
    return self:ClientHas(self.FEATURE_CUTAWAY)
end

function ns:CutawayWanted()
    return (self.db.enabled and self.db.cutaway and not self.cameraEdit) and true or false
end

-- True while the cutaway is doing the job indoors, so the camera has no need
-- to collide with anything. False too if the library has given up on it
-- (its tenth value: the renderer does not take the changed projection).
function ns:CutawayWorks()
    if not (self:CutawayWanted() and self:ClientCanCutAway()) then
        return false
    end
    return select(10, ARPG_CutawayInfo()) ~= 1
end

function ns:ApplyCutaway()
    if not self:ClientCanCutAway() then
        return
    end
    if self:CutawayWanted() then
        ARPG_SetCutaway(self.db.cutawayHeight, self.db.cutawayOutdoors)
    else
        ARPG_SetCutaway()
    end
end

---------------------------------------------------------------------------
-- Applying state
---------------------------------------------------------------------------

-- Brings CVars, camera and bindings in line with the saved settings.
-- Bindings cannot change in combat, so that part waits for combat to end.
function ns:Sync(quiet)
    local on = self.db.enabled
    if on then
        self:ApplyCVars()
    else
        self:RestoreCVars()
    end
    self:ApplyCutaway()
    self:ApplyClientCamera()
    self:ApplyGroundClicks()
    self:StopHold()

    if InCombatLockdown() then
        if not quiet and not self.syncPending then
            ns.Print("You're in combat - the controls will switch when combat ends.")
        end
        self.syncPending = true
        return false
    end
    self.syncPending = nil

    self:RefreshInput()
    if on and not self.applied then
        self:ApplyCamera()
    end
    self.applied = on
    return true
end

function ns:SetEnabled(on)
    on = on and true or false
    self.db.enabled = on
    if not on then
        self.cameraEdit = nil
    end
    if not self:Sync() then
        return -- in combat: Sync has said so, and finishes afterwards
    end

    if on then
        local move = self:ClientCanHoldMove() and "Hold the left mouse button to move" or "Left-click moves and interacts"
        ns.Print("Mode |cff20ff20on|r. " .. move .. ", scroll zoom is locked"
            .. (self.db.moveLocked and ", WASD is locked." or "."))
        if not self:ClientCanLockCamera() and not self.char.hasView then
            ns.Print("No camera saved yet: type /arpg camedit, set the height and angle you want, then /arpg savecam.")
        end
    else
        ns.Print("Mode |cffff2020off|r. Your normal controls are back.")
    end
end

function WotLKARPG_Toggle() -- used by Bindings.xml
    ns:SetEnabled(not ns.db.enabled)
end

---------------------------------------------------------------------------
-- Events
---------------------------------------------------------------------------

local frame = CreateFrame("Frame")
ns.frame = frame
frame:SetScript("OnEvent", function(_, event, ...)
    ns[event](ns, ...)
end)
frame:RegisterEvent("ADDON_LOADED")
frame:RegisterEvent("PLAYER_LOGIN")
frame:RegisterEvent("PLAYER_ENTERING_WORLD")
frame:RegisterEvent("PLAYER_REGEN_ENABLED")
frame:RegisterEvent("UPDATE_BINDINGS")

-- Walking through a doorway fires no event, so indoors is polled.
local sinceIndoorCheck = 0
frame:SetScript("OnUpdate", function(_, elapsed)
    sinceIndoorCheck = sinceIndoorCheck + elapsed
    if sinceIndoorCheck < 0.25 then return end
    sinceIndoorCheck = 0
    if ns.ready and ns.cameraLocked and ns:WantsCollision() ~= ns.cameraCollides then
        ns:ApplyClientCamera()
    end
end)

function ns:ADDON_LOADED(name)
    if name ~= ADDON then return end
    frame:UnregisterEvent("ADDON_LOADED")

    local char = WotLKARPGCharDB or {}
    local account = WotLKARPGDB
    if not account then
        account = {}
        -- Up to 0.2.1 the settings were kept per character. The first
        -- character to log in after that brings its own along, except that a
        -- mode left off is not carried over: it now starts on.
        for key in pairs(defaults) do
            if char[key] ~= nil and (key ~= "enabled" or char[key]) then
                account[key] = char[key]
            end
        end
        -- The old rotation slider could not be dragged back onto 0: a degree
        -- or two left behind by it was never meant.
        if account.camYaw and math.abs(account.camYaw) <= 2 then
            account.camYaw = 0
        end
    end
    for key, value in pairs(defaults) do
        char[key] = nil
        if account[key] == nil then
            account[key] = value
        end
    end
    for key, value in pairs(characterDefaults) do
        if char[key] == nil then
            char[key] = value
        end
    end
    WotLKARPGDB, WotLKARPGCharDB = account, char
    self.db, self.char = account, char
end

function ns:PLAYER_LOGIN()
    self.ready = true
    self:Sync(true)
end

-- Loading screens can move the camera; put the saved view back.
function ns:PLAYER_ENTERING_WORLD()
    if self.ready and self.db.enabled then
        self:ApplyClientCamera()
        self:ApplyCamera()
    end
end

function ns:PLAYER_REGEN_ENABLED()
    if self.syncPending and self:Sync(true) then
        ns.Print("Mode " .. (self.db.enabled and "|cff20ff20on|r." or "|cffff2020off|r."))
    end
end

-- The player rebound a movement or camera key: block the new key instead.
function ns:UPDATE_BINDINGS()
    if self.ready and self.db.enabled and self:InputChanged() then
        self:Sync(true)
    end
end

---------------------------------------------------------------------------
-- /arpg
---------------------------------------------------------------------------

local function onOff(flag, on, off)
    return flag and (on or "on") or (off or "off")
end

function ns:PrintStatus()
    local db = self.db
    ns.Print(format("Addon %s - mode %s, WASD %s.", self.version, onOff(db.enabled),
        onOff(db.moveLocked, "locked", "unlocked")))
    if self:HasClient() then
        ns.Print(format("Client library %s - hold-to-move %s, camera lock %s.", tostring((ARPG_ClientVersion())),
            onOff(self:ClientCanHoldMove(), "working", "NOT working"),
            onOff(self:ClientCanLockCamera(), "working", "NOT working")))
        ns.Print(format("Camera: rotation %d, angle %d, distance %d.", db.camYaw, db.camPitch, db.camDistance))
        self:PrintCameraProof()
        self:PrintCutawayProof()
        if self:ClientCanGroundClick() then
            ns.Print(format("Clicks on roofs and treetops go to the ground: %s (above %d yards).",
                onOff(db.groundClicks, "yes", "no"), db.groundClickHeight))
        end
        if not (self:ClientCanHoldMove() and self:ClientCanLockCamera()) then
            ns.Print("Something is off: please send WotLK-ARPG.log from the game folder.")
        end
    else
        ns.Print("Client library not loaded: one click per move, and dragging can still turn the camera.")
        ns.Print("Camera view " .. onOff(self.char.hasView, "saved.", "not saved."))
    end
    local left, right = GetBindingAction("BUTTON1"), GetBindingAction("BUTTON2")
    if left ~= "CAMERAORSELECTORMOVE" or right ~= "TURNORACTION" then
        ns.Print(format("Your saved mouse bindings are not the defaults (left: %s, right: %s). /arpg resetmouse puts them back.",
            left ~= "" and left or "none", right ~= "" and right or "none"))
    end
end

-- Says whether the client is really using the locked camera, from the
-- library's own count of the camera updates it has changed.
function ns:PrintCameraProof()
    if type(ARPG_CameraInfo) ~= "function" then
        return
    end
    local asked, angleRuns, angleLocked, distanceRuns, distanceLocked = ARPG_CameraInfo()
    if asked ~= 1 then
        ns.Print("The camera is not locked at the moment.")
    elseif angleLocked > 0 and distanceLocked > 0 then
        ns.Print(format("The camera lock is in effect: applied in %d of %d camera updates since the UI loaded.",
            distanceLocked, distanceRuns))
    else
        ns.Print(format("|cffff2020The camera lock is asked for but is not taking effect|r (%d and %d camera "
            .. "updates seen, %d and %d locked). Please send this line and WotLK-ARPG.log.",
            angleRuns, distanceRuns, angleLocked, distanceLocked))
    end
end

-- The same for the cutaway: what the library is doing with it right now.
function ns:PrintCutawayProof()
    local db = self.db
    local fallback = db.indoorCollision and "the camera moves in when a wall is in the way" or "the camera stays put"
    if not self:ClientCanCutAway() then
        ns.Print(format("Indoors: %s. (No cutaway: it needs client library 0.3.0 or later with all its hooks in.)",
            fallback))
        return
    end
    if not db.cutaway then
        ns.Print(format("Indoors: %s. The cutaway is switched off.", fallback))
        return
    end
    local asked, cutting, height, frames, cut, skipped, rooms, indoors, _, refused, overhead, between =
        ARPG_CutawayInfo()
    if asked ~= 1 then
        ns.Print("Cutaway: not in use at the moment.")
        return
    end
    -- What the library found around the character: this decides the cut.
    local view = (between or 0) > 0 and format("line of sight blocked %.1f yd up", between) or "line of sight clear"
    local where
    if indoors == 1 then
        where = format("indoors, %s, %s", (overhead or 0) > 0 and format("ceiling %.1f yd up", overhead)
            or "no ceiling found", view)
    else
        where = "outdoors, " .. view
    end
    if refused == 1 then
        ns.Print("|cffff2020Cutaway: the game's renderer does not take it|r (it needs Direct3D 9). Indoors "
            .. fallback .. " instead. Please send WotLK-ARPG.log.")
    elseif cutting == 1 then
        ns.Print(format("Cutaway: cutting %.1f yd above your feet (%s), %d rooms shown.", height, where, rooms))
    else
        ns.Print(format("Cutaway: on, nothing to cut right now (%s).", where))
    end
    if refused ~= 1 then
        if skipped > 0 and cut == 0 then
            ns.Print(format("|cffff2020The cutaway is asked for but is not taking effect|r (%d frames, %d skipped). "
                .. "Please send this line and WotLK-ARPG.log.", frames, skipped))
        else
            ns.Print(format("  Drawn cut in %d of %d frames since the UI loaded%s.", cut, frames,
                skipped > 0 and format(" (%d skipped)", skipped) or ""))
        end
    end
end

function ns:PrintHelp()
    ns.Print("Commands:")
    ns.Print("  /arpg - turn the mode on or off (also: /arpg on, /arpg off)")
    ns.Print("  /arpg wasd - lock or unlock WASD movement (in combat use the key binding)")
    ns.Print("  /arpg options - all settings (also: /arpg angle 60, /arpg distance 22, /arpg rotation 0)")
    ns.Print("  /arpg cut - roofs off indoors, on or off (/arpg cut 4: wall height; /arpg cut outdoors: outdoors too, on or off)")
    ns.Print("  /arpg ground - roof and treetop clicks go to the ground, on or off (/arpg ground 3 sets the height)")
    ns.Print("  /arpg sound - hear the world from the camera or from the character")
    ns.Print("  /arpg camedit - free the camera, mouse and scroll wheel for a look around")
    ns.Print("  /arpg status - show the current state")
    ns.Print("  /arpg resetmouse - restore the default left and right mouse bindings")
    if not self:ClientCanLockCamera() then
        ns.Print("  /arpg savecam - save the current camera as the ARPG camera; /arpg cam snaps back to it")
    end
    ns.Print("Keys for the mode and the WASD toggle are under Key Bindings > WotLK-ARPG.")
end

-- /arpg angle 55 and friends: a number within sensible limits.
local cameraSettings = {
    angle = { key = "camPitch", low = 10, high = 89, label = "Camera angle" },
    pitch = { key = "camPitch", low = 10, high = 89, label = "Camera angle" },
    distance = { key = "camDistance", low = 5, high = 100, label = "Camera distance" },
    dist = { key = "camDistance", low = 5, high = 100, label = "Camera distance" },
    rotation = { key = "camYaw", low = -180, high = 180, label = "Screen rotation" },
    facing = { key = "camYaw", low = -180, high = 180, label = "Screen rotation" },
    yaw = { key = "camYaw", low = -180, high = 180, label = "Screen rotation" },
}

local function setCameraSetting(setting, text)
    local value = tonumber(text)
    if not value then
        ns.Print(format("%s is %d. Give a number from %d to %d to change it.", setting.label,
            ns.db[setting.key], setting.low, setting.high))
        return
    end
    value = math.max(setting.low, math.min(setting.high, floor(value + 0.5)))
    ns.db[setting.key] = value
    ns:ApplyClientCamera()
    ns.Print(format("%s set to %d.", setting.label, value))
    if not ns:ClientCanLockCamera() then
        ns.Print("This needs the client library, which is not loaded.")
    end
end

-- /arpg ground: toggle; /arpg ground 3: on, from that many yards up.
function ns:SetGroundClicks(text)
    local height = tonumber(text)
    if height then
        self.db.groundClickHeight = math.max(1, math.min(30, floor(height + 0.5)))
        self.db.groundClicks = true
    else
        self.db.groundClicks = not self.db.groundClicks
    end
    self:ApplyGroundClicks()
    if self.db.groundClicks then
        ns.Print(format("Move clicks more than %d yards above the character go to the ground under the cursor.",
            self.db.groundClickHeight))
    else
        ns.Print("Move clicks land wherever the cursor is, roofs included.")
    end
    if not self:ClientCanGroundClick() then
        ns.Print("This needs the client library (0.2.1 or later), which is not loaded.")
    end
end

-- /arpg cut: toggle; /arpg cut 4: on, at that height; /arpg cut outdoors:
-- toggle the outdoor part; /arpg cut on, /arpg cut off.
function ns:SetCutaway(text)
    local db = self.db
    local height = tonumber(text)
    if height then
        db.cutawayHeight = math.max(2, math.min(15, floor(height + 0.5)))
        db.cutaway = true
    elseif text == "outdoors" or text == "outdoor" then
        db.cutawayOutdoors = not db.cutawayOutdoors
    elseif text == "on" or text == "off" then
        db.cutaway = text == "on"
    else
        db.cutaway = not db.cutaway
    end
    self:ApplyCutaway()
    self:ApplyClientCamera()
    if db.cutaway then
        ns.Print(format("Cutaway on: indoors, what is more than %d yards above your feet is left out%s.",
            db.cutawayHeight, db.cutawayOutdoors and "; outdoors, so is a building that hides your character" or ""))
    else
        ns.Print("Cutaway off: buildings are drawn whole.")
    end
    if not self:ClientCanCutAway() then
        ns.Print("This needs the client library (0.3.0 or later), which is not loaded.")
    elseif not db.enabled then
        ns.Print("It takes effect while the mode is on (/arpg).")
    end
end

function ns:SetSoundAtCamera(on)
    self.db.soundAtCamera = on and true or false
    if self.db.enabled then
        self:ApplyCVars()
    end
    ns.Print(self.db.soundAtCamera and "Sound is heard from the camera: left and right match the screen."
        or "Sound is heard from the character.")
end

SLASH_WOTLKARPG1 = "/arpg"
SlashCmdList["WOTLKARPG"] = function(msg)
    local cmd, arg = strlower(strtrim(msg or "")):match("^(%S*)%s*(.-)$")
    if cmd == "" or cmd == "toggle" then
        ns:SetEnabled(not ns.db.enabled)
    elseif cmd == "on" then
        ns:SetEnabled(true)
    elseif cmd == "off" then
        ns:SetEnabled(false)
    elseif cmd == "wasd" then
        ns:ToggleMoveLock()
    elseif cameraSettings[cmd] then
        setCameraSetting(cameraSettings[cmd], arg)
    elseif cmd == "cut" or cmd == "cutaway" then
        ns:SetCutaway(arg)
    elseif cmd == "ground" then
        ns:SetGroundClicks(arg)
    elseif cmd == "sound" then
        ns:SetSoundAtCamera(not ns.db.soundAtCamera)
    elseif cmd == "options" or cmd == "config" then
        ns:OpenOptions()
    elseif cmd == "camedit" then
        ns:SetCameraEdit(not ns.cameraEdit)
    elseif cmd == "savecam" then
        ns:SaveCamera()
    elseif cmd == "cam" then
        ns:ApplyCamera(true)
    elseif cmd == "status" then
        ns:PrintStatus()
    elseif cmd == "resetmouse" then
        ns:ResetMouseBindings()
    else
        ns:PrintHelp()
    end
end
