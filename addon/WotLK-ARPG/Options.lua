--[[
    Options.lua: the settings panel (Interface > AddOns > WotLK-ARPG, or
    /arpg options). Every control has a short label and says what it does to
    the game in its tooltip. Changes take effect at once.

    Sliders: drag, or roll the mouse wheel for single steps; right-click puts
    a slider back to its default. The Defaults button resets the whole panel.

    The panel the client gives an addon is about 400 units wide and 415 high
    (3.3.5a, whatever the UI scale). Everything here has to fit in that:
    the camera sliders sit two to a row for it.
]]

local ADDON, ns = ...

local panel = CreateFrame("Frame", "WotLKARPGOptions", UIParent)
panel.name = "WotLK-ARPG"
panel:Hide()

local title = panel:CreateFontString(nil, "ARTWORK", "GameFontNormalLarge")
title:SetPoint("TOPLEFT", 16, -16)
title:SetText("WotLK-ARPG")

local note = panel:CreateFontString(nil, "ARTWORK", "GameFontHighlightSmall")
note:SetPoint("TOPLEFT", title, "BOTTOMLEFT", 0, -4)
note:SetWidth(376)
note:SetHeight(24) -- two lines at most
note:SetJustifyH("LEFT")
note:SetJustifyV("TOP")

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
    slider:SetWidth(spec.width or 165)
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
    check:SetPoint("TOPLEFT", spec.x or 16, spec.y)
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

heading("Camera", -66)

makeSlider("camPitch", {
    x = 24, y = -98, low = 10, high = 89, lowText = "Flat", highText = "Top-down",
    text = function(value) return format("Camera angle: %d°", value) end,
    tooltip = "How steeply the camera looks down at your character.",
    apply = applyCamera,
})

makeSlider("camDistance", {
    x = 214, y = -98, low = 5, high = 100, lowText = "Near", highText = "Far",
    text = function(value) return format("Camera distance: %d yd", value) end,
    tooltip = "How far the camera is from your character. Further away shows more of the world, smaller.",
    apply = applyCamera,
})

makeSlider("camYaw", {
    x = 24, y = -142, low = -180, high = 180, lowText = "-180°", highText = "180°", snap = 5,
    text = function(value)
        if value == 0 then
            return "Rotation: none (north is up)"
        end
        return format("Rotation: %d°", value)
    end,
    tooltip = "Turns the whole view. At 0, north is at the top of the screen, as on the minimap.",
    apply = applyCamera,
})

---------------------------------------------------------------------------
-- Buildings
---------------------------------------------------------------------------

heading("Buildings and caves", -180)

makeCheck("cutaway", {
    y = -196, label = "Indoors, show only the inside of the building",
    tooltip = "Wherever the game has you indoors (where you cannot ride), you look into the rooms from above: "
        .. "the roof and the walls on the camera's side are left out, and so is everything outdoors, which "
        .. "goes black. Clicks go through what is left out.",
    apply = applyCutaway,
})

makeSlider("cutawayHeight", {
    x = 48, y = -240, width = 200, low = 2, high = 15, lowText = "Low walls", highText = "Full height",
    text = function(value) return format("Wall height: %d yd", value) end,
    tooltip = "Indoors, whatever is higher than this above your character's feet is cut away: beams, the "
        .. "floor upstairs, the tops of the walls. Low gives the clearest view. All the way up, walls stand "
        .. "to the ceiling. Under a lower ceiling the cut is made just below the ceiling.",
    apply = applyCutaway,
})

makeCheck("cutawayOutdoors", {
    y = -274, label = "Outdoors, open up the building that hides you",
    tooltip = "Outdoors, when a roof, an archway or a wall hides your character, it is opened along your line "
        .. "of sight: what covers your character on screen is not drawn, with a strip to either side as wide as "
        .. "the free space beside it. What is behind or beside your character stays whole. (Client libraries "
        .. "older than 0.7.0 cut buildings level instead.)",
    apply = applyCutaway,
})

makeCheck("indoorCollision", {
    y = -299, label = "Otherwise, move the camera in indoors",
    tooltip = "Without the inside view: indoors the camera comes closer whenever a wall or ceiling is in the "
        .. "way, so you see your character instead of the roof.",
    apply = applyCamera,
})

---------------------------------------------------------------------------
-- Clicks and sound
---------------------------------------------------------------------------

heading("Clicks and sound", -332)

makeCheck("groundClicks", {
    y = -348, label = "Roof clicks walk to the ground below",
    tooltip = "A move click that lands on a roof or a treetop takes your character to the ground under the "
        .. "cursor instead.",
    apply = function() ns:ApplyGroundClicks() end,
})

makeCheck("soundAtCamera", {
    y = -373, label = "Sound follows the screen",
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
    setCheckEnabled(checks.cutaway, cutaway, "Needs the client library, version 0.5.0 or later.")
    setSliderEnabled(sliders.cutawayHeight, cutaway and db.cutaway)
    setCheckEnabled(checks.cutawayOutdoors, cutaway and db.cutaway)
    setCheckEnabled(checks.indoorCollision, camera and not (cutaway and db.cutaway), camera
        and "Not needed while the inside view is on." or "Needs the client library.")
    setCheckEnabled(checks.groundClicks, ns:ClientCanGroundClick(), "Needs the client library.")

    if not ns:HasClient() then
        -- Two lines is all the room there is: about 140 characters.
        note:SetText("The client library (WotLK-ARPG.dll) is not loaded, and most of this needs it. "
            .. "Meanwhile: /arpg camedit, /arpg savecam.")
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
