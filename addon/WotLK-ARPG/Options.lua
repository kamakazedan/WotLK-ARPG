--[[
    Options.lua: the settings panel (Interface > AddOns > WotLK-ARPG, or
    /arpg options). Every control has a short label and says what it does to
    the game in its tooltip. Changes take effect at once.

    Sliders: drag, or roll the mouse wheel for single steps; right-click puts
    a slider back to its default. The Defaults button resets the whole panel.
]]

local ADDON, ns = ...

local panel = CreateFrame("Frame", "WotLKARPGOptions", UIParent)
panel.name = "WotLK-ARPG"
panel:Hide()

local title = panel:CreateFontString(nil, "ARTWORK", "GameFontNormalLarge")
title:SetPoint("TOPLEFT", 16, -16)
title:SetText("WotLK-ARPG")

local note = panel:CreateFontString(nil, "ARTWORK", "GameFontHighlightSmall")
note:SetPoint("TOPLEFT", title, "BOTTOMLEFT", 0, -6)
note:SetWidth(380)
note:SetJustifyH("LEFT")

local function heading(text, offsetY)
    local label = panel:CreateFontString(nil, "ARTWORK", "GameFontNormal")
    label:SetPoint("TOPLEFT", 16, offsetY)
    label:SetText(text)
end

local SLIDER_HINT = "\n\nMouse wheel: one step at a time. Right-click: back to the default."

local sliders = {}

-- A slider bound to one whole number in the saved settings.
--   text(value)  the line above the slider
--   snap         dragging moves in steps of this size (the mouse wheel always moves by 1)
local function makeSlider(key, spec)
    local name = "WotLKARPGOptions_" .. key
    local slider = CreateFrame("Slider", name, panel, "OptionsSliderTemplate")
    slider:SetPoint("TOPLEFT", spec.x or 24, spec.y)
    slider:SetWidth(spec.width or 260)
    slider:SetMinMaxValues(spec.low, spec.high)
    slider:SetValueStep(1)
    _G[name .. "Low"]:SetText(spec.lowText)
    _G[name .. "High"]:SetText(spec.highText)
    slider.tooltipText = spec.tooltip .. SLIDER_HINT

    local function clamp(value)
        return math.max(spec.low, math.min(spec.high, value))
    end

    function slider.ShowValue(value)
        _G[name .. "Text"]:SetText(spec.text(value))
    end

    slider:SetScript("OnValueChanged", function(_, value)
        value = floor(value + 0.5)
        if slider.dragging and spec.snap then
            -- A slider is only so many pixels wide: without this, dragging
            -- one with a wide range can step right over a value such as 0.
            value = clamp(floor(value / spec.snap + 0.5) * spec.snap)
        end
        slider.ShowValue(value)
        -- Ignore the changes made while the panel fills itself in.
        if ns.db and not panel.refreshing and ns.db[key] ~= value then
            ns.db[key] = value
            spec.apply()
        end
    end)

    slider:SetScript("OnMouseDown", function(_, button)
        if button == "LeftButton" then
            slider.dragging = true
        end
    end)
    slider:SetScript("OnMouseUp", function(_, button)
        slider.dragging = nil
        if not ns.db then return end
        if button == "RightButton" and slider:IsEnabled() then
            slider:SetValue(ns.defaults[key])
        else
            slider:SetValue(ns.db[key]) -- the thumb goes to the value the drag settled on
        end
    end)
    slider:EnableMouseWheel(true)
    slider:SetScript("OnMouseWheel", function(_, delta)
        if ns.db and slider:IsEnabled() then
            slider:SetValue(clamp(ns.db[key] + (delta > 0 and 1 or -1)))
        end
    end)

    sliders[key] = slider
    return slider
end

local checks = {}

-- A tick box bound to one on/off value in the saved settings.
local function makeCheck(key, spec)
    local name = "WotLKARPGOptions_" .. key
    local check = CreateFrame("CheckButton", name, panel, "InterfaceOptionsCheckButtonTemplate")
    check:SetPoint("TOPLEFT", spec.x or 20, spec.y)
    _G[name .. "Text"]:SetText(spec.label)
    check.tooltipText = spec.tooltip
    check.setFunc = function(value)
        if ns.db then
            ns.db[key] = (value == "1")
            spec.apply()
            panel.refresh() -- some controls depend on others
        end
    end
    checks[key] = check
    return check
end

local function applyCamera()
    ns:ApplyClientCamera()
end

local function applyCutaway()
    ns:ApplyCutaway()
    ns:ApplyClientCamera() -- whether the camera collides indoors depends on the cutaway
end

---------------------------------------------------------------------------
-- Camera
---------------------------------------------------------------------------

heading("Camera", -76)

makeSlider("camPitch", {
    y = -112, low = 10, high = 89, lowText = "Flat", highText = "Top-down",
    text = function(value) return format("Camera angle: %d°", value) end,
    tooltip = "How steeply the camera looks down at your character.",
    apply = applyCamera,
})

makeSlider("camDistance", {
    y = -162, low = 5, high = 100, lowText = "Near", highText = "Far",
    text = function(value) return format("Camera distance: %d yd", value) end,
    tooltip = "How far the camera is from your character. Further away shows more of the world, smaller.",
    apply = applyCamera,
})

makeSlider("camYaw", {
    y = -212, low = -180, high = 180, lowText = "-180°", highText = "180°", snap = 5,
    text = function(value)
        if value == 0 then
            return "Screen rotation: none (north is up)"
        end
        return format("Screen rotation: %d°", value)
    end,
    tooltip = "Turns the whole view. At 0, north is at the top of the screen, as on the minimap.",
    apply = applyCamera,
})

---------------------------------------------------------------------------
-- Buildings
---------------------------------------------------------------------------

heading("Buildings and caves", -250)

makeCheck("cutaway", {
    y = -268, label = "Hide roofs and upper floors indoors",
    tooltip = "Indoors, nothing above your character's head is drawn, so you look into the room from above. "
        .. "Clicks go straight through what is hidden.",
    apply = applyCutaway,
})

makeSlider("cutawayHeight", {
    x = 52, y = -316, width = 232, low = 2, high = 15, lowText = "Low walls", highText = "High walls",
    text = function(value) return format("Wall height indoors: %d yd", value) end,
    tooltip = "How much of the walls is left standing. Lower shows more of the floor behind them. Higher keeps "
        .. "more of the walls, and of creatures taller than the cut. Under a low ceiling the cut is made "
        .. "just below the ceiling; in a tall room, just below whatever is in the way.",
    apply = applyCutaway,
})

makeCheck("cutawayOutdoors", {
    x = 44, y = -350, label = "Outdoors too, when a building hides your character",
    tooltip = "When an archway, bridge or building comes between the camera and your character, it is cut "
        .. "away just far enough to see past it. The same in halls taller than the camera. Trees and rocks "
        .. "are left alone.",
    apply = applyCutaway,
})

makeCheck("indoorCollision", {
    y = -380, label = "Otherwise, move the camera in indoors",
    tooltip = "Without the cutaway: indoors the camera comes closer whenever a wall or ceiling is in the way, "
        .. "so you see your character instead of the roof.",
    apply = applyCamera,
})

---------------------------------------------------------------------------
-- Clicks and sound
---------------------------------------------------------------------------

heading("Clicks and sound", -416)

makeCheck("groundClicks", {
    y = -434, label = "Roof clicks walk to the ground below",
    tooltip = "A move click that lands on a roof or a treetop takes your character to the ground under the "
        .. "cursor instead.",
    apply = function() ns:ApplyGroundClicks() end,
})

makeCheck("soundAtCamera", {
    y = -462, label = "Sound follows the screen",
    tooltip = "Something on the right of the screen is heard on the right. Off: sound is heard from where "
        .. "your character stands and faces, as the game normally does it.",
    apply = function() if ns.db.enabled then ns:ApplyCVars() end end,
})

---------------------------------------------------------------------------
-- Filling the panel in
---------------------------------------------------------------------------

local function setCheckEnabled(check, enabled, requirement)
    if enabled then
        BlizzardOptionsPanel_CheckButton_Enable(check, true)
    else
        BlizzardOptionsPanel_CheckButton_Disable(check)
    end
    check.tooltipRequirement = (not enabled) and requirement or nil
end

local function setSliderEnabled(slider, enabled)
    if enabled then
        BlizzardOptionsPanel_Slider_Enable(slider)
        _G[slider:GetName() .. "Text"]:SetVertexColor(HIGHLIGHT_FONT_COLOR.r, HIGHLIGHT_FONT_COLOR.g,
            HIGHLIGHT_FONT_COLOR.b)
    else
        BlizzardOptionsPanel_Slider_Disable(slider)
    end
end

local function refresh()
    local db = ns.db
    if not db then return end
    panel.refreshing = true
    for key, slider in pairs(sliders) do
        slider:SetValue(db[key])
        slider.ShowValue(db[key]) -- SetValue is silent when the value is already there
    end
    for key, check in pairs(checks) do
        check:SetChecked(db[key])
    end
    panel.refreshing = nil

    local camera = ns:ClientCanLockCamera()
    local cutaway = ns:ClientCanCutAway()
    setSliderEnabled(sliders.camPitch, camera)
    setSliderEnabled(sliders.camDistance, camera)
    setSliderEnabled(sliders.camYaw, camera)
    setCheckEnabled(checks.cutaway, cutaway, "Needs the client library, version 0.3.0 or later.")
    setSliderEnabled(sliders.cutawayHeight, cutaway and db.cutaway)
    setCheckEnabled(checks.cutawayOutdoors, cutaway and db.cutaway)
    setCheckEnabled(checks.indoorCollision, camera and not (cutaway and db.cutaway), camera
        and "Not needed while roofs are hidden." or "Needs the client library.")
    setCheckEnabled(checks.groundClicks, ns:ClientCanGroundClick(), "Needs the client library.")

    if not ns:HasClient() then
        note:SetText("Most of these need the client library (WotLK-ARPG.dll), which is not loaded. Without it, "
            .. "set the camera up with /arpg camedit and /arpg savecam.")
    elseif db.enabled then
        note:SetText("The mode is on. Everything here takes effect as you change it.")
    else
        note:SetText("The mode is off (/arpg turns it on). These settings apply while it is on.")
    end
end

panel.refresh = refresh
panel:SetScript("OnShow", refresh)

-- The Defaults button. The mode itself, and whether WASD is locked, are not
-- settings of this panel and stay as they are.
panel.default = function()
    local db = ns.db
    if not db then return end
    for key, value in pairs(ns.defaults) do
        if key ~= "enabled" and key ~= "moveLocked" then
            db[key] = value
        end
    end
    ns:Sync(true)
end

InterfaceOptions_AddCategory(panel)

function ns:OpenOptions()
    InterfaceOptionsFrame_OpenToCategory(panel)
end
