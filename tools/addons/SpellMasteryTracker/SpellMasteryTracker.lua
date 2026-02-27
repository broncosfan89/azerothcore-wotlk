local ADDON_PREFIX = "SMT"
local VOLLEY_BASE_SPELL_ID = 1510
local VOLLEY_BASE_NAME = GetSpellInfo(VOLLEY_BASE_SPELL_ID)
local TIER_NAMES = {
    [1] = "Iron",
    [2] = "Bronze",
    [3] = "Silver",
    [4] = "Gold",
    [5] = "Diamond"
}

SMT_DB = SMT_DB or {}

local trackerFrame
local spellText
local tierText
local xpText
local updateText
local progressBar
local currentSpellId

local function RoundNearest(value)
    return math.floor(value + 0.5)
end

local function RegisterPrefix()
    if C_ChatInfo and C_ChatInfo.RegisterAddonMessagePrefix then
        C_ChatInfo.RegisterAddonMessagePrefix(ADDON_PREFIX)
    elseif RegisterAddonMessagePrefix then
        RegisterAddonMessagePrefix(ADDON_PREFIX)
    end
end

local function ParsePayload(payload)
    if type(payload) ~= "string" or payload == "" then
        return nil
    end

    local parsed = {}
    for pair in string.gmatch(payload, "[^;]+") do
        local key, value = string.match(pair, "^([^=]+)=(.+)$")
        if key and value then
            parsed[key] = value
        end
    end

    local spellId = tonumber(parsed.SPELL)
    if not spellId then
        return nil
    end

    return {
        spellId = spellId,
        tier = tonumber(parsed.TIER) or 1,
        lvl = tonumber(parsed.LVL) or 1,
        xp = tonumber(parsed.XP) or 0,
        next = tonumber(parsed.NEXT) or 0,
        ts = time()
    }
end

local function ResolveAddonMessage(...)
    local arg1, arg2, arg3, arg4 = ...

    if arg1 == ADDON_PREFIX then
        return arg2
    end

    if type(arg1) == "string" then
        local prefix, payload = string.match(arg1, "^([^\t]+)\t(.+)$")
        if prefix == ADDON_PREFIX then
            return payload
        end
    end

    return nil
end

local function GetTierName(tier)
    return TIER_NAMES[tier] or ("Tier " .. tostring(tier or 0))
end

local function GetEffectiveTierLevel(progressTier, progressLevel, targetTier)
    if progressTier > targetTier then
        return 10
    end

    if progressTier == targetTier then
        return progressLevel
    end

    return 0
end

local function IsVolleyTooltipSpell(spellName, spellId)
    if spellId and spellId == VOLLEY_BASE_SPELL_ID then
        return true
    end

    if VOLLEY_BASE_NAME and spellName and spellName == VOLLEY_BASE_NAME then
        return true
    end

    return false
end

local function AddVolleyMasteryTooltip(tooltip)
    if not tooltip or not tooltip.GetSpell then
        return
    end

    local spellName, _, spellId = tooltip:GetSpell()
    if not IsVolleyTooltipSpell(spellName, spellId) then
        return
    end

    local state = SMT_DB and SMT_DB[VOLLEY_BASE_SPELL_ID]
    if not state then
        return
    end

    local tier = tonumber(state.tier) or 1
    local level = tonumber(state.lvl) or 1
    local xp = tonumber(state.xp) or 0
    local nextXp = tonumber(state.next) or 0

    local ironLevel = GetEffectiveTierLevel(tier, level, 1)
    local bronzeLevel = GetEffectiveTierLevel(tier, level, 2)
    local silverLevel = GetEffectiveTierLevel(tier, level, 3)
    local goldLevel = GetEffectiveTierLevel(tier, level, 4)
    local diamondLevel = GetEffectiveTierLevel(tier, level, 5)

    local totalLevels = ironLevel + bronzeLevel + silverLevel + goldLevel + diamondLevel
    local damagePct = totalLevels * 1.5
    local radiusMultiplier = 1.0 + (0.5 * (bronzeLevel / 10.0))

    local tickInterval = 1000
    if silverLevel > 0 then
        local reductionMs = RoundNearest((1000 - 750) * (silverLevel / 10.0))
        tickInterval = math.max(750, 1000 - reductionMs)
    end

    local diamondBurstPct = 0
    if diamondLevel > 0 then
        diamondBurstPct = 5.0 + ((diamondLevel - 1) * (10.0 / 9.0))
    end

    tooltip:AddLine(" ")
    tooltip:AddLine("|cff66ccffSpell Mastery: Volley|r")
    tooltip:AddDoubleLine("Tier", string.format("%s %d", TIER_NAMES[tier] or "Unknown", level))
    tooltip:AddDoubleLine("Progress", string.format("%d / %d XP", xp, nextXp))
    tooltip:AddLine(string.format("Damage Bonus: +%.1f%%", damagePct))
    tooltip:AddLine(string.format("Radius: x%.2f", radiusMultiplier))
    tooltip:AddLine(string.format("Tick Interval: %d ms", tickInterval))

    if goldLevel > 0 then
        tooltip:AddLine(string.format("Gold: Serpent spread to %d targets", goldLevel))
    else
        tooltip:AddLine("Gold: Serpent spread inactive")
    end

    if diamondLevel > 0 then
        tooltip:AddLine(string.format("Diamond Burst: %.1f%% of hit", diamondBurstPct))
    else
        tooltip:AddLine("Diamond Burst: inactive")
    end

    tooltip:Show()
end

local function RefreshFrame()
    if not trackerFrame or not trackerFrame:IsShown() then
        return
    end

    if not currentSpellId or not SMT_DB[currentSpellId] then
        spellText:SetText("Spell: No data")
        tierText:SetText("Tier: -")
        xpText:SetText("XP: -")
        updateText:SetText("Last update: -")
        progressBar:SetMinMaxValues(0, 1)
        progressBar:SetValue(0)
        progressBar.Text:SetText("0%")
        return
    end

    local entry = SMT_DB[currentSpellId]
    local spellName = GetSpellInfo(currentSpellId) or ("Spell " .. tostring(currentSpellId))
    local nextXp = math.max(entry.next or 0, 0)
    local xp = math.max(entry.xp or 0, 0)

    spellText:SetText("Spell: " .. spellName)
    tierText:SetText(string.format("Tier: %s  Level: %d", GetTierName(entry.tier), entry.lvl or 1))

    if nextXp > 0 then
        local clampedXp = math.min(xp, nextXp)
        local pct = math.floor((clampedXp / nextXp) * 100 + 0.5)
        progressBar:SetMinMaxValues(0, nextXp)
        progressBar:SetValue(clampedXp)
        progressBar.Text:SetText(pct .. "%")
        xpText:SetText(string.format("XP: %d / %d", clampedXp, nextXp))
    else
        progressBar:SetMinMaxValues(0, 1)
        progressBar:SetValue(1)
        progressBar.Text:SetText("MAX")
        xpText:SetText("XP: MAX")
    end

    local stamp = entry.ts and date("%H:%M:%S", entry.ts) or "-"
    updateText:SetText("Last update: " .. stamp)
end

local function BuildFrame()
    if trackerFrame then
        return
    end

    trackerFrame = CreateFrame("Frame", "SMTTrackerFrame", UIParent)
    trackerFrame:SetSize(280, 150)
    trackerFrame:SetPoint("CENTER", UIParent, "CENTER", 0, 180)
    trackerFrame:SetMovable(true)
    trackerFrame:EnableMouse(true)
    trackerFrame:RegisterForDrag("LeftButton")
    trackerFrame:SetScript("OnDragStart", trackerFrame.StartMoving)
    trackerFrame:SetScript("OnDragStop", trackerFrame.StopMovingOrSizing)
    trackerFrame:SetBackdrop({
        bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background",
        edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
        tile = true,
        tileSize = 16,
        edgeSize = 16,
        insets = { left = 4, right = 4, top = 4, bottom = 4 }
    })
    trackerFrame:SetBackdropColor(0.05, 0.05, 0.05, 0.9)
    trackerFrame:Hide()

    local title = trackerFrame:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
    title:SetPoint("TOP", trackerFrame, "TOP", 0, -10)
    title:SetText("Spell Mastery Tracker")

    spellText = trackerFrame:CreateFontString(nil, "OVERLAY", "GameFontHighlight")
    spellText:SetPoint("TOPLEFT", trackerFrame, "TOPLEFT", 12, -36)
    spellText:SetJustifyH("LEFT")

    tierText = trackerFrame:CreateFontString(nil, "OVERLAY", "GameFontHighlight")
    tierText:SetPoint("TOPLEFT", spellText, "BOTTOMLEFT", 0, -8)
    tierText:SetJustifyH("LEFT")

    progressBar = CreateFrame("StatusBar", nil, trackerFrame)
    progressBar:SetSize(250, 18)
    progressBar:SetPoint("TOPLEFT", tierText, "BOTTOMLEFT", 0, -12)
    progressBar:SetStatusBarTexture("Interface\\TARGETINGFRAME\\UI-StatusBar")
    progressBar:SetStatusBarColor(0.95, 0.45, 0.12)
    progressBar:SetMinMaxValues(0, 1)
    progressBar:SetValue(0)

    local bg = progressBar:CreateTexture(nil, "BACKGROUND")
    bg:SetAllPoints(progressBar)
    bg:SetTexture("Interface\\TARGETINGFRAME\\UI-StatusBar")
    bg:SetVertexColor(0.2, 0.2, 0.2, 0.8)

    progressBar.Text = progressBar:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    progressBar.Text:SetPoint("CENTER", progressBar, "CENTER", 0, 0)

    xpText = trackerFrame:CreateFontString(nil, "OVERLAY", "GameFontHighlight")
    xpText:SetPoint("TOPLEFT", progressBar, "BOTTOMLEFT", 0, -8)
    xpText:SetJustifyH("LEFT")

    updateText = trackerFrame:CreateFontString(nil, "OVERLAY", "GameFontDisable")
    updateText:SetPoint("TOPLEFT", xpText, "BOTTOMLEFT", 0, -8)
    updateText:SetJustifyH("LEFT")

    RefreshFrame()
end

SLASH_SMT1 = "/smt"
SlashCmdList.SMT = function()
    BuildFrame()
    if trackerFrame:IsShown() then
        trackerFrame:Hide()
    else
        trackerFrame:Show()
        RefreshFrame()
    end
end

local eventFrame = CreateFrame("Frame")
eventFrame:RegisterEvent("PLAYER_LOGIN")
eventFrame:RegisterEvent("CHAT_MSG_ADDON")
eventFrame:SetScript("OnEvent", function(_, event, ...)
    if event == "PLAYER_LOGIN" then
        RegisterPrefix()
        BuildFrame()
        if GameTooltip and GameTooltip.HookScript then
            GameTooltip:HookScript("OnTooltipSetSpell", AddVolleyMasteryTooltip)
        end
        return
    end

    local payload = ResolveAddonMessage(...)
    if not payload then
        return
    end

    local parsed = ParsePayload(payload)
    if not parsed then
        return
    end

    SMT_DB[parsed.spellId] = {
        tier = parsed.tier,
        lvl = parsed.lvl,
        xp = parsed.xp,
        next = parsed.next,
        ts = parsed.ts
    }
    currentSpellId = parsed.spellId
    RefreshFrame()
end)
