--[[
    Options.lua: the camera settings panel (Interface > AddOns > WotLK-ARPG,
    or /arpg options). The camera only changes from here or from /arpg.
]]

local ADDON, ns = ...

local panel = CreateFrame("Frame", "WotLKARPGOptions", UIParent)
panel.name = "WotLK-ARPG"
panel:Hide()

local title = panel:CreateFontString(nil, "ARTWORK", "GameFontNormalLarge")
title:SetPoint("TOPLEFT", 16, -16)
title:SetText("WotLK-ARPG")

local note = panel:CreateFontString(nil, "ARTWORK", "GameFontHighlightSmall")
note:SetPoint("TOPLEFT", title, "BOTTOMLEFT", 0, -8)
note:SetWidth(380)
note:SetJustifyH("LEFT")

local sliders = {}

-- A slider bound to one whole number in the saved settings.
local function makeSlider(key, label, low, high, suffix, offsetY)
    local name = "WotLKARPGOptions_" .. key
    local slider = CreateFrame("Slider", name, panel, "OptionsSliderTemplate")
    slider:SetPoint("TOPLEFT", 24, offsetY)
    slider:SetWidth(260)
    slider:SetMinMaxValues(low, high)
    slider:SetValueStep(1)
    _G[name .. "Low"]:SetText(low .. suffix)
    _G[name .. "High"]:SetText(high .. suffix)

    function slider.ShowValue(value)
        _G[name .. "Text"]:SetText(format("%s: %d%s", label, value, suffix))
    end

    slider:SetScript("OnValueChanged", function(_, value)
        value = floor(value + 0.5)
        slider.ShowValue(value)
        -- Ignore the changes made while the panel fills itself in.
        if ns.db and not panel.refreshing and ns.db[key] ~= value then
            ns.db[key] = value
            ns:ApplyClientCamera()
        end
    end)
    sliders[key] = slider
    return slider
end

makeSlider("camPitch", "Angle (higher looks more straight down)", 10, 89, "", -90)
makeSlider("camDistance", "Distance from the character", 5, 100, " yd", -150)
makeSlider("camYaw", "Facing (0 keeps north at the top)", -180, 180, "", -210)

local indoor = CreateFrame("CheckButton", "WotLKARPGOptions_Indoor", panel, "InterfaceOptionsCheckButtonTemplate")
indoor:SetPoint("TOPLEFT", 20, -262)
_G["WotLKARPGOptions_IndoorText"]:SetText("Indoors, let the camera collide so the character stays in view")
indoor.setFunc = function(value)
    if ns.db then
        ns.db.indoorCollision = (value == "1")
        ns:ApplyClientCamera()
    end
end

local function refresh()
    if not ns.db then return end
    panel.refreshing = true
    for key, slider in pairs(sliders) do
        slider:SetValue(ns.db[key])
        slider.ShowValue(ns.db[key]) -- SetValue is silent when the value is already there
    end
    indoor:SetChecked(ns.db.indoorCollision)
    panel.refreshing = nil

    if ns:ClientCanLockCamera() then
        note:SetText("The camera is locked to these values while the mode is on (/arpg).")
    else
        note:SetText("These settings need the client library (WotLK-ARPG.dll), which is not loaded. "
            .. "Without it, set the camera up with /arpg camedit and /arpg savecam.")
    end
end

panel.refresh = refresh
panel:SetScript("OnShow", refresh)

InterfaceOptions_AddCategory(panel)

function ns:OpenOptions()
    InterfaceOptionsFrame_OpenToCategory(panel)
end
