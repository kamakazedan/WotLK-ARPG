--[[
    WotLK-ARPG - Diablo-style camera and controls for the 3.3.5a client.

    Core.lua: saved variables, the mode switch, events and the /arpg command.
    Camera.lua, Input.lua and Options.lua add the pieces the switch drives.

    Everything the mode changes is reversible: CVars are snapshotted and put
    back, and every key or mouse change is an override binding that vanishes
    when the mode is turned off. The saved key bindings are never touched.

    The addon works alone on a stock client. With the client library
    (WotLK-ARPG.dll, see client/ in the repository) it also gets hold-to-move
    and a camera locked to a set direction, angle and distance.
]]

local ADDON, ns = ...

WotLKARPG = ns -- handy for /run while testing
ns.version = GetAddOnMetadata(ADDON, "Version") or "dev"

BINDING_HEADER_WOTLKARPG = "WotLK-ARPG"
BINDING_NAME_WOTLKARPG_TOGGLE = "Toggle ARPG mode"
BINDING_NAME_WOTLKARPG_HOLDMOVE = "Move and interact (hold to keep moving)"
_G["BINDING_NAME_CLICK WotLKARPGMoveLock:LeftButton"] = "Toggle WASD movement"

local defaults = {
    enabled = false,        -- ARPG mode is on for this character
    moveLocked = true,      -- movement keys are disabled while the mode is on
    hasView = false,        -- the camera view slot holds the ARPG camera (no client library)
    camYaw = 0,             -- degrees; 0 looks north, so north is up on screen
    camPitch = 55,          -- degrees above the horizon the camera looks down from
    camDistance = 30,       -- yards from the character
    indoorCollision = true, -- let the camera collide while the character is indoors
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
    self:ApplyClientCamera()
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
        if not self:ClientCanLockCamera() and not self.db.hasView then
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

    WotLKARPGCharDB = WotLKARPGCharDB or {}
    self.db = WotLKARPGCharDB
    for key, value in pairs(defaults) do
        if self.db[key] == nil then
            self.db[key] = value
        end
    end
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
        ns.Print(format("Camera: facing %d, angle %d, distance %d, collides indoors: %s.", db.camYaw, db.camPitch,
            db.camDistance, onOff(db.indoorCollision, "yes", "no")))
        if not (self:ClientCanHoldMove() and self:ClientCanLockCamera()) then
            ns.Print("Something is off: please send WotLK-ARPG.log from the game folder.")
        end
    else
        ns.Print("Client library not loaded: one click per move, and dragging can still turn the camera.")
        ns.Print("Camera view " .. onOff(db.hasView, "saved.", "not saved."))
    end
    local left, right = GetBindingAction("BUTTON1"), GetBindingAction("BUTTON2")
    if left ~= "CAMERAORSELECTORMOVE" or right ~= "TURNORACTION" then
        ns.Print(format("Your saved mouse bindings are not the defaults (left: %s, right: %s). /arpg resetmouse puts them back.",
            left ~= "" and left or "none", right ~= "" and right or "none"))
    end
end

function ns:PrintHelp()
    ns.Print("Commands:")
    ns.Print("  /arpg - turn the mode on or off (also: /arpg on, /arpg off)")
    ns.Print("  /arpg wasd - lock or unlock WASD movement (in combat use the key binding)")
    ns.Print("  /arpg options - camera settings (also: /arpg angle 55, /arpg distance 30, /arpg facing 0)")
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
    facing = { key = "camYaw", low = -180, high = 180, label = "Camera facing" },
    yaw = { key = "camYaw", low = -180, high = 180, label = "Camera facing" },
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
