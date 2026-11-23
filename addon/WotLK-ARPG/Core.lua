--[[
    WotLK-ARPG - Diablo-style camera and controls for the 3.3.5a client.

    Core.lua: saved variables, the mode switch, events and the /arpg command.
    Camera.lua, Input.lua and Options.lua add the pieces the switch drives.

    Nothing the mode changes outlasts it. The client's settings (CVars) are
    the mode's only while a session lasts: the player's own go back when the
    mode is turned off and whenever the UI shuts down, before the client
    saves them, so no other client the account logs in from ever sees the
    mode's. Every key or mouse change is an override binding that vanishes
    with the mode. The saved key bindings are never touched.

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
    cutaway = true,         -- indoors, show only the inside of the building, looked into from above
    cutawayHeight = 4,      -- yards above the character's feet at which that view is cut off
    cutawayOutdoors = false, -- outdoors, cut open the parts of a building that hide the character
    indoorCollision = true, -- without the inside view: indoors, let the camera move in when a wall is in the way
    groundClicks = true,    -- move clicks on roofs and treetops go to the ground under the cursor
    groundClickHeight = 3,  -- yards above the character's feet that still count as somewhere to walk
    soundAtCamera = true,   -- hear the world from the camera, so left and right match the screen
}
ns.defaults = defaults

-- Kept per character (WotLKARPGCharDB), because the client keeps the thing
-- it describes per character: whether the camera view slot holds the ARPG
-- camera. (Up to 0.3.0 also savedCVars: see AdoptSavedCVars in Camera.lua.)
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
ns.FEATURE_CUTAWAY = 16     -- up to library 0.3.0 this alone: the world cut open, outdoors and all
ns.FEATURE_INSIDE_VIEW = 32 -- from 0.4.0: buildings shown from inside
ns.FEATURE_ROOMS = 64       -- from 0.5.0: rooms told from streets; ARPG_CutawayInfo says what was found

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

-- The inside view (the "cutaway" of the settings and of the library's
-- function names): while the character is in a room, the library shows the
-- building from inside, looked into from above, with nothing of the outdoor
-- world, and cuts off whatever is higher than a set height. "In a room" is
-- wherever the game itself has the character indoors. The library decides
-- frame by frame what there is to do; this only says whether it may.
function ns:ClientCanCutAway()
    return self:ClientHas(self.FEATURE_ROOMS)
end

function ns:CutawayWanted()
    return (self.db.enabled and self.db.cutaway and not self.cameraEdit) and true or false
end

-- True while the inside view is doing the job indoors, so the camera has no
-- need to collide with anything.
function ns:CutawayWorks()
    return self:CutawayWanted() and self:ClientCanCutAway()
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
frame:RegisterEvent("PLAYER_LOGOUT")
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
    -- 0.4.0 shows buildings from inside. Slicing the world outdoors, which
    -- was on by default before and leaves hollow buildings standing about,
    -- is now something to ask for.
    if (account.revision or 1) < 2 then
        account.cutawayOutdoors = false
        account.revision = 2
    end
    for key, value in pairs(characterDefaults) do
        if char[key] == nil then
            char[key] = value
        end
    end
    WotLKARPGDB, WotLKARPGCharDB = account, char
    self.db, self.char = account, char

    -- A snapshot of the player's own CVars from 0.3.0 or before: looked at
    -- at login, when the client has loaded the account's settings.
    self.legacyCVars, char.savedCVars = char.savedCVars, nil
end

function ns:PLAYER_LOGIN()
    self.ready = true
    if self.legacyCVars then
        self:AdoptSavedCVars(self.legacyCVars)
        self.legacyCVars = nil
    end
    self:Sync(true)
end

-- Logging out, exiting, reloading the UI. The client is about to save its
-- settings, the account's to the server as well: it writes them right after
-- this event (0x00528F00 raises it at 0x00529025 and writes the settings
-- caches from 0x0052908E on, and nowhere else). So it gets the player's own.
-- The mode stays on; the next login applies its values again.
function ns:PLAYER_LOGOUT()
    self:RestoreCVars()
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

-- The same for the inside view: what the library is doing right now, and
-- what it found around the character that made it decide so.
function ns:PrintCutawayProof()
    local db = self.db
    local fallback = db.indoorCollision and "the camera moves in when a wall is in the way" or "the camera stays put"
    if not self:ClientCanCutAway() then
        ns.Print(format("Indoors: %s. (No inside view: it needs client library 0.5.0 or later with all its "
            .. "hooks in.)", fallback))
        return
    end
    if not db.cutaway then
        ns.Print(format("Indoors: %s. The inside view is switched off.", fallback))
        return
    end
    -- The last five are nil with a library older than 0.6.0.
    local asked, cutting, height, frames, cut, skipped, rooms, indoors, _, refused, overhead, between, inside,
        insideFrames, draws, group, groupFlags, area, doorsOut, parts, sceneryOut, creaturesOut, canChoose,
        canFilter = ARPG_CutawayInfo()
    if asked ~= 1 then
        ns.Print("Inside view: not in use at the moment.")
        return
    end
    local ceiling = (overhead or 0) > 0 and format("ceiling %.1f yd up", overhead) or "no ceiling found"
    local cutText = cutting == 1 and format("cut %.1f yd above your feet", height) or "nothing cut"
    if inside == 1 then
        ns.Print(format("Inside view: %d room%s shown from inside, %s, %s.", rooms, rooms == 1 and "" or "s",
            ceiling, cutText))
        if canFilter == 0 then
            ns.Print("  |cffffd200The library cannot tell where scenery and creatures stand in this game file|r: "
                .. "what the game files under a room is drawn, also when it stands outside the walls.")
        elseif (sceneryOut or 0) + (creaturesOut or 0) > 0 then
            ns.Print(format("  Left out for standing outside those rooms: scenery %d, creatures and objects %d.",
                sceneryOut or 0, creaturesOut or 0))
        end
    elseif indoors == 1 then
        -- The game says the character is indoors, but its lookup of the
        -- room under the character came back empty or with no room.
        ns.Print(format("|cffffd200Inside view: this building is not found from inside|r, so the world is "
            .. "drawn, sliced open (%s, %s, %s, %d rooms).", ceiling, (between or 0) > 0
            and format("line of sight blocked %.1f yd up", between) or "line of sight clear", cutText, rooms))
    elseif cutting == 1 then
        local hidden = (between or 0) > 0 and format("a building hides you %.1f yd up", between)
            or "nothing hides you any more"
        if parts == nil or canChoose == 0 then
            ns.Print(format("Outdoors: %s; the world is %s.", hidden, cutText))
            if canChoose == 0 then
                ns.Print("  |cffffd200The library cannot tell the pieces of the world apart in this game file|r, "
                    .. "so everything on screen is cut.")
            end
        elseif parts < 0 then
            ns.Print(format("Outdoors: %s; which part of it could not be told, so the whole world is %s.", hidden,
                cutText))
        elseif parts == 0 then
            ns.Print(format("Outdoors: %s; the cut is on its way out.", hidden))
        else
            ns.Print(format("Outdoors: %s; %d part%s of it, with the rooms inside, %s. The rest is whole.", hidden,
                parts, parts == 1 and "" or "s", cutText))
            if (sceneryOut or 0) + (creaturesOut or 0) > 0 then
                ns.Print(format("  Left out for sitting on those parts above the cut: scenery %d, creatures and "
                    .. "objects %d.", sceneryOut or 0, creaturesOut or 0))
            end
        end
    else
        ns.Print("Inside view: on. You are outdoors, and nothing is cut.")
    end
    -- What the game has under the character's feet: the numbers to send
    -- along when a place shows the wrong picture.
    if (group or -1) < 0 then
        ns.Print("  Under your feet: open ground, or the outside of a building.")
    else
        ns.Print(format("  Under your feet: part %d of a building (flags %X, area %s), which the game counts as %s.%s",
            group, bit.band(groupFlags or 0, 0x7FFFFFFF), (area or -1) < 0 and "none" or format("%X", area),
            inside == 1 and "indoors" or "outdoors",
            (inside == 1 and (doorsOut or 0) > 0) and format(" %d way%s out not shown.", doorsOut,
                doorsOut == 1 and "" or "s") or ""))
    end
    if refused == 1 then
        ns.Print("|cffff2020The game's renderer does not take the cut|r (it needs Direct3D 9): beams and the "
            .. "floor upstairs stay in the picture. Please send WotLK-ARPG.log.")
    elseif skipped > 0 and cut == 0 then
        ns.Print(format("|cffff2020The cut is asked for but is not taking effect|r (%d frames, %d skipped). "
            .. "Please send this line and WotLK-ARPG.log.", frames, skipped))
    end
    ns.Print(format("  Since the UI loaded: %d frames, %d drawn from inside a building, %d cut%s%s.", frames,
        insideFrames or 0, cut, (draws or 0) > 0 and format(" (%d draws in the last)", draws) or "",
        skipped > 0 and format(", %d skipped", skipped) or ""))
end

function ns:PrintHelp()
    ns.Print("Commands:")
    ns.Print("  /arpg - turn the mode on or off (also: /arpg on, /arpg off)")
    ns.Print("  /arpg wasd - lock or unlock WASD movement (in combat use the key binding)")
    ns.Print("  /arpg options - all settings (also: /arpg angle 60, /arpg distance 22, /arpg rotation 0)")
    ns.Print("  /arpg cut - buildings shown from inside, on or off (/arpg cut 4: wall height; /arpg cut outdoors: open up a building that hides you outdoors, too)")
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
        ns.Print(format("Inside view on: indoors you see only the inside of the building, up to %d yards "
            .. "above your feet.%s", db.cutawayHeight, db.cutawayOutdoors
            and " Outdoors, a building that hides your character is opened up." or ""))
    else
        ns.Print("Inside view off: buildings are drawn whole.")
    end
    if not self:ClientCanCutAway() then
        ns.Print("This needs the client library (0.5.0 or later), which is not loaded.")
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
