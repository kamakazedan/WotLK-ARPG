--[[
    WotLK-ARPG - Diablo-style camera and controls for the 3.3.5a client.

    Core.lua: saved variables, the mode switch, events and the /arpg command.
    Camera.lua and Input.lua add the pieces the switch turns on and off.

    Everything the mode changes is reversible: CVars are snapshotted and put
    back, and every key or mouse change is an override binding that vanishes
    when the mode is turned off. The saved key bindings are never touched.
]]

local ADDON, ns = ...

WotLKARPG = ns -- handy for /run while testing
ns.version = GetAddOnMetadata(ADDON, "Version") or "dev"

BINDING_HEADER_WOTLKARPG = "WotLK-ARPG"
BINDING_NAME_WOTLKARPG_TOGGLE = "Toggle ARPG mode"
_G["BINDING_NAME_CLICK WotLKARPGMoveLock:LeftButton"] = "Toggle WASD movement"

local defaults = {
    enabled = false,   -- ARPG mode is on for this character
    moveLocked = true, -- movement keys are disabled while the mode is on
    hasView = false,   -- the camera view slot holds the ARPG camera
}

function ns.Print(msg)
    DEFAULT_CHAT_FRAME:AddMessage("|cffc41f3bARPG|r " .. tostring(msg))
end

-- The client library (see docs/DESIGN.md) registers this global when present.
function ns:HasClient()
    return type(ARPG_ClientVersion) == "function"
end

---------------------------------------------------------------------------
-- Applying state
---------------------------------------------------------------------------

-- Brings CVars and bindings in line with the saved settings. Bindings cannot
-- change in combat, so in combat this waits for PLAYER_REGEN_ENABLED.
function ns:Sync(quiet)
    if InCombatLockdown() then
        if not quiet and not self.syncPending then
            ns.Print("You're in combat - this will apply when combat ends.")
        end
        self.syncPending = true
        return false
    end
    self.syncPending = nil

    local on = self.db.enabled
    if on then
        self:ApplyCVars()
    else
        self:RestoreCVars()
    end
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
        return -- in combat: Sync has said so, and applies it afterwards
    end

    if on then
        ns.Print("Mode |cff20ff20on|r. Left-click moves and interacts, scroll zoom is locked"
            .. (self.db.moveLocked and ", WASD is locked." or "."))
        if not self.db.hasView then
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
    ns.Print(format("v%s - mode %s, WASD %s, camera %s.", self.version, onOff(db.enabled),
        onOff(db.moveLocked, "locked", "unlocked"), onOff(db.hasView, "saved", "not saved")))
    if self:HasClient() then
        ns.Print("Client library " .. tostring(ARPG_ClientVersion()) .. " loaded.")
    else
        ns.Print("Client library not installed: one click per move, and dragging can still turn the camera.")
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
    ns.Print("  /arpg camedit - free the mouse and scroll wheel to set up the camera")
    ns.Print("  /arpg savecam - save the current camera as the ARPG camera")
    ns.Print("  /arpg cam - snap back to the saved camera")
    ns.Print("  /arpg status - show the current state")
    ns.Print("  /arpg resetmouse - restore the default left and right mouse bindings")
    ns.Print("Keys for the mode and the WASD toggle are under Key Bindings > WotLK-ARPG.")
end

SLASH_WOTLKARPG1 = "/arpg"
SlashCmdList["WOTLKARPG"] = function(msg)
    local cmd = strlower(strtrim(msg or ""))
    if cmd == "" or cmd == "toggle" then
        ns:SetEnabled(not ns.db.enabled)
    elseif cmd == "on" then
        ns:SetEnabled(true)
    elseif cmd == "off" then
        ns:SetEnabled(false)
    elseif cmd == "wasd" then
        ns:ToggleMoveLock()
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
