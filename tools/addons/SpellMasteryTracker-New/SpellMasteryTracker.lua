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

local function RoundNearest(value)
    return math.floor(value + 0.5)
end

local function ParseMasteryPayload(message)
    if type(message) ~= "string" or message == "" then
        return nil
    end

    local data = {}
    for key, value in message:gmatch("([A-Z_]+)=([^;]+)") do
        data[key] = value
    end

    local spellId = tonumber(data.SPELL or "")
    local tier = tonumber(data.TIER or "")
    local level = tonumber(data.LVL or "")
    local xp = tonumber(data.XP or "") or 0
    local nextXp = tonumber(data.NEXT or "") or 0

    if not spellId or not tier or not level then
        return nil
    end

    return {
        spellId = spellId,
        tier = math.max(1, math.min(5, tier)),
        level = math.max(1, math.min(10, level)),
        xp = math.max(0, xp),
        nextXp = math.max(0, nextXp),
        ts = time()
    }
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

    local tickInterval = nil
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

    if tickInterval then
        tooltip:AddLine(string.format("Tick Interval: %d ms", tickInterval))
    else
        tooltip:AddLine("Tick Interval: 1000 ms")
    end

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

if not SMT_DB then
    SMT_DB = {}
end

local frame = CreateFrame("Frame", "SMT_Frame")
frame:RegisterEvent("PLAYER_LOGIN")
frame:RegisterEvent("CHAT_MSG_ADDON")
frame:SetScript("OnEvent", function(_, event, ...)
    if event == "PLAYER_LOGIN" then
        if RegisterAddonMessagePrefix then
            RegisterAddonMessagePrefix(ADDON_PREFIX)
        end

        GameTooltip:HookScript("OnTooltipSetSpell", AddVolleyMasteryTooltip)
        return
    end

    if event == "CHAT_MSG_ADDON" then
        local prefix, message = ...
        if prefix ~= ADDON_PREFIX then
            return
        end

        local payload = ParseMasteryPayload(message)
        if not payload then
            return
        end

        SMT_DB[payload.spellId] = {
            tier = payload.tier,
            lvl = payload.level,
            xp = payload.xp,
            next = payload.nextXp,
            ts = payload.ts
        }
    end
end)
