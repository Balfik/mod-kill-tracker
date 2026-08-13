-- KillTrackerWaypoint
-- Listens for system chat messages from the "Hunt Chronicler" NPC and, if
-- they contain "/way ...", automatically feeds everything after "/way "
-- straight into TomTom's REAL parser (SlashCmdList["TOMTOM_WAY"]) - exactly
-- as if the player had typed the command themselves. This makes both the
-- old format ("/way X Y name" - current zone, daily contract) and the new
-- one ("/way Zone_Name X Y name" - any zone, boss list) work -
-- TomTom already knows how to fuzzy-match a zone by name, we don't need to compute anything.

local function StripColors(msg)
    msg = msg:gsub("|c%x%x%x%x%x%x%x%x", "")
    msg = msg:gsub("|r", "")
    return msg
end

-- Cheap check BEFORE gsub/match - CHAT_MSG_SYSTEM fires on EVERY system
-- message (login, MOTD, resets, level-ups, etc.), not just ours. Without
-- this filter we'd run StripColors+match on every such line, including
-- non-ASCII text at login - a plain substring search (no patterns) here is
-- much cheaper and safer.
local function OnSystemMessage(self, event, msg)
    if not msg or not msg:find("/way", 1, true) then return end

    local ok, err = pcall(function()
        local plain = StripColors(msg)
        local wayArgs = plain:match("/way%s+(.+)$")
        if not wayArgs then return end

        if not SlashCmdList or not SlashCmdList["TOMTOM_WAY"] then
            DEFAULT_CHAT_FRAME:AddMessage("|cffff0000KillTrackerWaypoint:|r TomTom addon not found - install TomTom for pins to be set automatically.")
            return
        end

        SlashCmdList["TOMTOM_WAY"](wayArgs)
    end)

    if not ok then
        DEFAULT_CHAT_FRAME:AddMessage("|cffff0000KillTrackerWaypoint:|r error handling /way (" .. tostring(err) .. "), pin not set.")
    end
end

-- CHAT_MSG_SYSTEM is only registered after PLAYER_LOGIN (not immediately at
-- file load) - the same caution used in KillTrackerHUD: don't touch the
-- event/frame system while the engine is still in the process of entering
-- the world.
local frame = CreateFrame("Frame")
frame:RegisterEvent("PLAYER_LOGIN")
frame:SetScript("OnEvent", function(self, event, ...)
    if event == "PLAYER_LOGIN" then
        self:UnregisterEvent("PLAYER_LOGIN")
        self:RegisterEvent("CHAT_MSG_SYSTEM")
        return
    end
    OnSystemMessage(self, event, ...)
end)
