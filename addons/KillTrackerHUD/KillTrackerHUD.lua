-- KillTrackerHUD
-- Client-side companion to the server-side "Hunt Chronicler" NPC
-- (mod-kill-tracker). The server sends three payload system messages
-- (kill_milestones.cpp: KT_SendHudUpdate) - stats, daily contract, and
-- leaderboard - each with its own marker at the start of the line. The
-- addon catches them via ChatFrame_AddMessageEventFilter, hides them from
-- chat, and displays them in a panel with several tabs.
--
-- Commands:
--   /kthud        - show/hide the panel
--   /kthud reset  - reset the panel to its default position
--
-- HISTORY: v1 used a raw CHAT_MSG_ADDON packet (unstable, see the git/
-- chat history). v2 deferred building the UI to PLAYER_LOGIN. v3 moved to
-- CHAT_MSG_SYSTEM + ChatFrame_AddMessageEventFilter (a standard, stable
-- Blizzard API). v4: three tabs (Stats/Contract/Leaderboard) - same stable
-- channel, just three markers instead of one. v5: a fourth "Weekly" tab -
-- weekly bonus contract (a tougher boss, 2x XP + 400 gold), same protocol
-- approach. v6: the "Leaderboard" tab REMOVED (the leaderboard stays in
-- the NPC gossip menu), a dungeons/raids-cleared line added to Stats
-- instead. v7: a [New Contract] button on the Contract tab (client->server
-- via the ".kthudreroll" dot-command - ChatHandler::ParseCommands
-- intercepts it before it's broadcast as normal chat, so nearby players
-- see nothing); a new "Bonuses" tab - summed % by category (all stats/
-- hit chance/XP), data arrives in the SAME STATS marker (version 4).

local MARKER_STATS    = "##KTHUD##"
local MARKER_CONTRACT = "##KTCONTRACT##"
local MARKER_WEEKLY   = "##KTWEEKLY##"
-- The server still sends the leaderboard (for compatibility/possible
-- future use) - the HUD tab is gone, so this marker is now only
-- recognized and hidden from chat; the data goes nowhere.
local MARKER_BOARD    = "##KTBOARD##"

-- "Rate Holiday" (mod-rate-holiday) - a server-driven full-screen banner,
-- the same safe CHAT_MSG_SYSTEM protocol, a separate pair of markers.
local MARKER_RH_START = "##RATEHOLSTART##"
local MARKER_RH_STOP  = "##RATEHOLSTOP##"

-- Must match KT_CONTRACT_GOLD_REWARD/KT_WEEKLY_GOLD_REWARD in
-- kill_milestones.h (the server doesn't send these numbers over the
-- protocol - they're fixed and the same for everyone, so the constants are
-- just duplicated here for display, not computed dynamically).
local CONTRACT_GOLD_REWARD = 100
local WEEKLY_GOLD_REWARD   = 400

-- PANEL_H: was 214, temporarily raised to 270 for the 7-line [Bonuses]
-- tab - now [Bonuses] scrolls on its own (ScrollFrame, independent of the
-- overall panel height), so the height is back closer to the original,
-- just slightly bigger (per the request to "shrink the height a bit").
local PANEL_W, PANEL_H = 320, 226

KillTrackerHUD_DB = KillTrackerHUD_DB or {}

local frame, tabButtons, tabContents
local statLines
local bonusLines
local contractState = {}
local weeklyState = {}
local bonusState = {}
local countdownTicker = 0

-- Flash/sound on a new tier or daily contract completion (per the user's
-- request - "so I don't miss it by accident"). lastSeenTier/
-- lastSeenContractCompleted deliberately START as nil (not 0/false) -
-- the first value received after a /reload or login is only REMEMBERED,
-- with no flash (otherwise every /reload with an already-earned tier/
-- contract would trigger as "just received").
local lastSeenTier = nil
local lastSeenContractCompleted = nil
-- The same nil-start rule for the three new stats progress bars -
-- elite/rare, quests, daily quota.
local lastSeenEliteBonusPct = nil
local lastSeenQuestTier = nil
local lastSeenDailyCompletions = nil
local flashOverlay, flashElapsed = nil, 0
local FLASH_DURATION = 1.2 -- seconds, full fade-out

-- Seconds -> "HH:MM:SS". IMPORTANT: declared here, BEFORE BuildUI - it's
-- called by the OnUpdate handler defined INSIDE BuildUI (further down in
-- the file); if FormatCountdown were declared after BuildUI, that call
-- would resolve to a non-existent GLOBAL function (Lua determines local
-- scope by declaration position in the text, not execution order) -
-- throwing "attempt to call a nil value" the first time the timer fires.
local function FormatCountdown(totalSeconds)
    if totalSeconds <= 0 then return "updating..." end
    local h = math.floor(totalSeconds / 3600)
    local m = math.floor((totalSeconds % 3600) / 60)
    local s = totalSeconds % 60
    return string.format("%02d:%02d:%02d", h, m, s)
end

-- A brief colored panel flash + sound - called from UpdateStats (new tier)
-- and UpdateContract (daily contract completed). The overlay itself
-- (flashOverlay) is created in BuildUI (needs `frame` to already exist),
-- so this is just a "is it ready yet" check - safe to call even before
-- the UI is built (it just does nothing).
local function TriggerFlash(r, g, b, soundFile)
    if not flashOverlay then return end
    flashOverlay:SetVertexColor(r, g, b)
    flashOverlay:SetAlpha(0.55)
    flashOverlay:Show()
    flashElapsed = 0
    if soundFile then
        pcall(PlaySoundFile, soundFile, "Master")
    end
end

local function SetActiveTab(index)
    for i, btn in ipairs(tabButtons) do
        local isActive = (i == index)
        -- IMPORTANT: Frame:SetShown() does NOT EXIST on the 3.3.5a client
        -- (added in much later clients) - calling this nonexistent method
        -- threw a Lua error right inside BuildUI(), and since this happened
        -- inside the same pcall as the ChatFrame_AddMessageEventFilter
        -- registration (which runs AFTER BuildUI() in the same block), the
        -- error aborted execution before the filter could register - hence
        -- the raw line spam in chat, the empty HUD, and the broken tabs.
        -- Using plain Show()/Hide() instead.
        if isActive then
            tabContents[i]:Show()
            btn.bg:SetTexture(0.25, 0.25, 0.25, 0.9)
        else
            tabContents[i]:Hide()
            btn.bg:SetTexture(0.08, 0.08, 0.08, 0.9)
        end
    end
    KillTrackerHUD_DB.activeTab = index
end

local function CreateTabButton(parent, index, text, x, width)
    local btn = CreateFrame("Button", nil, parent)
    btn:SetWidth(width or 80)
    btn:SetHeight(20)
    btn:SetPoint("TOPLEFT", parent, "TOPLEFT", x, -26)

    local bg = btn:CreateTexture(nil, "BACKGROUND")
    bg:SetAllPoints(btn)
    bg:SetTexture(0.08, 0.08, 0.08, 0.9)
    btn.bg = bg

    local label = btn:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    label:SetPoint("CENTER", btn, "CENTER", 0, 0)
    label:SetText(text)

    btn:SetScript("OnClick", function() SetActiveTab(index) end)
    return btn
end

-- Shared constructor for a thin StatusBar strip with a centered label -
-- reused for the tier/daily quota/elite/quest bars (previously duplicated
-- separately for each, now a single helper).
local function CreateProgressBar(parent, anchorFrame, yOffset, width, r, g, b)
    -- IMPORTANT: the border frame is created FIRST and made the PARENT of
    -- the bar (not the other way around, as before). This is why the bar
    -- used to look gray/washed out: both the StatusBar fill and our bg
    -- texture default to the "BACKGROUND" draw layer; textures created
    -- LATER on the same layer draw ON TOP of earlier ones - meaning the bg
    -- (dark, alpha 0.8), added AFTER SetStatusBarTexture, sat ON TOP of the
    -- colored fill itself and nearly killed the color. Now bg is
    -- deliberately in a separate child frame UNDER the bar (a lower layer
    -- by hierarchy), and the bar's fill is explicitly set to the
    -- "ARTWORK" layer (above BACKGROUND) - it's guaranteed to be on top.
    local border = CreateFrame("Frame", nil, parent)
    border:SetPoint("TOPLEFT", anchorFrame, "TOPLEFT", 3, yOffset + 1)
    border:SetWidth(width + 2)
    border:SetHeight(14)
    local borderTex = border:CreateTexture(nil, "BACKGROUND")
    borderTex:SetAllPoints(border)
    borderTex:SetTexture(0.55, 0.55, 0.55, 1)

    local bg = border:CreateTexture(nil, "ARTWORK")
    bg:SetPoint("TOPLEFT", border, "TOPLEFT", 1, -1)
    bg:SetPoint("BOTTOMRIGHT", border, "BOTTOMRIGHT", -1, 1)
    bg:SetTexture(0.08, 0.08, 0.08, 1)

    local bar = CreateFrame("StatusBar", nil, border)
    bar:SetPoint("TOPLEFT", border, "TOPLEFT", 1, -1)
    bar:SetPoint("BOTTOMRIGHT", border, "BOTTOMRIGHT", -1, 1)
    bar:SetStatusBarTexture("Interface\\TargetingFrame\\UI-StatusBar")
    bar:GetStatusBarTexture():SetDrawLayer("OVERLAY") -- guaranteed above bg
    bar:SetStatusBarColor(r, g, b)
    bar:SetMinMaxValues(0, 100)
    bar:SetValue(0)

    local text = bar:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    text:SetPoint("CENTER", bar, "CENTER", 0, 0)
    text:SetShadowColor(0, 0, 0, 1)
    text:SetShadowOffset(1, -1)

    return bar, text
end

-- [Stats] tab - WRAPPED in a ScrollFrame (per the request "don't grow the
-- window height") instead of growing PANEL_H further - the same approach
-- as [Bonuses] below (UIPanelScrollFrameTemplate + mouse wheel, a scroll
-- child with a generous fixed height).
local function BuildStatsTab(parent)
    local scrollFrame = CreateFrame("ScrollFrame", "KillTrackerHUDStatsScroll", parent, "UIPanelScrollFrameTemplate")
    scrollFrame:SetPoint("TOPLEFT", parent, "TOPLEFT", 0, 0)
    scrollFrame:SetPoint("BOTTOMRIGHT", parent, "BOTTOMRIGHT", -20, 0)

    scrollFrame:EnableMouseWheel(true)
    scrollFrame:SetScript("OnMouseWheel", function(self, delta)
        local newValue = self:GetVerticalScroll() - delta * 20
        if newValue < 0 then newValue = 0 end
        local maxScroll = self:GetVerticalScrollRange()
        if maxScroll and newValue > maxScroll then newValue = maxScroll end
        self:SetVerticalScroll(newValue)
    end)

    local content = CreateFrame("Frame", nil, scrollFrame)
    content:SetWidth(PANEL_W - 40)
    content:SetHeight(260)
    scrollFrame:SetScrollChild(content)

    -- y-offsets: each text line is 18px, each progress bar under its row
    -- takes another 18px (12px for the bar itself + spacing).
    local Y_TOTAL, Y_TIER, Y_TIERBAR = 0, -18, -36
    local Y_STREAK, Y_DAILY, Y_DAILYBAR = -54, -72, -90
    local Y_BOSSES, Y_ELITE, Y_ELITEBAR = -108, -126, -144
    local Y_QUEST, Y_QUESTBAR, Y_DUNGEONS = -162, -180, -198

    local STAT_Y = { Y_TOTAL, Y_TIER, Y_STREAK, Y_DAILY, Y_BOSSES, Y_ELITE, Y_QUEST, Y_DUNGEONS }

    statLines = {}
    for i = 1, 8 do
        local fs = content:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
        fs:SetPoint("TOPLEFT", content, "TOPLEFT", 4, STAT_Y[i])
        fs:SetJustifyH("LEFT")
        fs:SetWidth(PANEL_W - 44)
        statLines[i] = fs
    end

    -- IMPORTANT: field handles are written on the SCROLLFRAME (what this
    -- function returns and what lives in tabContents[1]), NOT on the
    -- inner content - otherwise UpdateStats (which reaches into
    -- tabContents[1].tierBar etc.) wouldn't find them.
    local barWidth = PANEL_W - 48
    scrollFrame.tierBar, scrollFrame.tierBarText     = CreateProgressBar(content, content, Y_TIERBAR, barWidth, 0.2, 0.6, 1.0)
    scrollFrame.dailyBar, scrollFrame.dailyBarText   = CreateProgressBar(content, content, Y_DAILYBAR, barWidth, 0.5, 0.5, 0.5)
    scrollFrame.eliteBar, scrollFrame.eliteBarText   = CreateProgressBar(content, content, Y_ELITEBAR, barWidth, 1.0, 0.5, 0.0)
    scrollFrame.questBar, scrollFrame.questBarText   = CreateProgressBar(content, content, Y_QUESTBAR, barWidth, 0.4, 0.8, 1.0)

    return scrollFrame
end

-- [Bonuses] tab - summed % BY STAT CATEGORY (not by source), as the user
-- asked: "total X% to all stats, Y% to hit chance" etc. Data arrives in
-- the SAME STATS marker as the Stats tab (UpdateStats below) - so it
-- updates in real time via the same push mechanism, no separate query needed.
--
-- WRAPPED in a ScrollFrame (per the request "if the info doesn't fit, add
-- a scrollbar") - there are 7 lines here, several of them multi-line
-- (word-wrap), so the total height is NOT fixed and depends on the
-- character/progress. UIPanelScrollFrameTemplate - the standard Blizzard
-- template with a scrollbar; returns the SCROLLFRAME itself (not the
-- inner content) - Show()/Hide() in SetActiveTab work with it the same as
-- with any other tab frame.
local function BuildBonusTab(parent)
    local scrollFrame = CreateFrame("ScrollFrame", "KillTrackerHUDBonusScroll", parent, "UIPanelScrollFrameTemplate")
    scrollFrame:SetPoint("TOPLEFT", parent, "TOPLEFT", 0, 0)
    scrollFrame:SetPoint("BOTTOMRIGHT", parent, "BOTTOMRIGHT", -20, 0) -- -20 to clear the scrollbar itself

    scrollFrame:EnableMouseWheel(true)
    scrollFrame:SetScript("OnMouseWheel", function(self, delta)
        local newValue = self:GetVerticalScroll() - delta * 20
        if newValue < 0 then newValue = 0 end
        local maxScroll = self:GetVerticalScrollRange()
        if maxScroll and newValue > maxScroll then newValue = maxScroll end
        self:SetVerticalScroll(newValue)
    end)

    -- Inner scroll child - a fixed GENEROUS height (400px) instead of
    -- measuring the real text height each time: if there's less content -
    -- just extra empty space at the bottom and the scrollbar stays inactive
    -- (standard template behavior); if there's more - scrolling works.
    local content = CreateFrame("Frame", nil, scrollFrame)
    content:SetWidth(PANEL_W - 40) -- -20 (tab's overall margin) - 20 (scrollbar)
    content:SetHeight(400)
    scrollFrame:SetScrollChild(content)

    local hint = content:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    hint:SetPoint("TOPLEFT", content, "TOPLEFT", 4, 0)
    hint:SetJustifyH("LEFT")
    hint:SetWidth(PANEL_W - 40)
    hint:SetText("|cff888888Total permanent bonuses (bosses+dungeons+elite+quests+quota):|r")
    content.hint = hint

    -- IMPORTANT: lines 5-7 are text, MULTI-LINE (word-wrap, height grows as
    -- needed - Master's Gift/kill tier can wrap to 2-3 lines depending on
    -- length). So each following line anchors relative to the BOTTOMLEFT of
    -- the previous one (not a fixed offset from the top of the panel) -
    -- otherwise long text in one line would overlap the next.
    bonusLines = {}
    local prevAnchor = hint
    local prevOffset = -4
    for i = 1, 7 do
        local fs = content:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
        fs:SetPoint("TOPLEFT", prevAnchor, "BOTTOMLEFT", 0, prevOffset)
        fs:SetJustifyH("LEFT")
        fs:SetWidth(PANEL_W - 40)
        bonusLines[i] = fs
        prevAnchor = fs
        prevOffset = -2
    end

    return scrollFrame
end

local function GoToContract()
    if not contractState.hasContract then
        DEFAULT_CHAT_FRAME:AddMessage("|cffffd700KillTrackerHUD:|r no contract data yet.")
        return
    end
    if not SlashCmdList or not SlashCmdList["TOMTOM_WAY"] then
        DEFAULT_CHAT_FRAME:AddMessage("|cffff0000KillTrackerHUD:|r install TomTom for pins to be set.")
        return
    end
    SlashCmdList["TOMTOM_WAY"](string.format("%.2f %.2f %s", contractState.x or 0, contractState.y or 0, contractState.name or "?"))
end

local function BuildContractTab(parent)
    local content = CreateFrame("Frame", nil, parent)
    content:SetAllPoints(parent)

    local text = content:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    text:SetPoint("TOPLEFT", content, "TOPLEFT", 4, 0)
    text:SetJustifyH("LEFT")
    text:SetWidth(PANEL_W - 20)
    text:SetHeight(80)
    content.text = text

    -- A dedicated line for a live countdown to the next contract (so the
    -- entire multi-line text doesn't need rebuilding every second).
    local countdown = content:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    countdown:SetPoint("TOPLEFT", content, "TOPLEFT", 4, -84)
    countdown:SetJustifyH("LEFT")
    countdown:SetWidth(PANEL_W - 20)
    content.countdown = countdown

    -- Two buttons side by side at the bottom - [Set Route] (left half) and
    -- [New Contract] (right half, reload - same as the button in the NPC
    -- gossip menu, just without visiting it). Width is computed so both fit
    -- with a small gap (same PANEL_W-20 previously used for one button,
    -- now split in half minus a 4px gap).
    local halfW = (PANEL_W - 20 - 4) / 2

    local goBtn = CreateFrame("Button", nil, content)
    goBtn:SetWidth(halfW)
    goBtn:SetHeight(22)
    goBtn:SetPoint("BOTTOMLEFT", content, "BOTTOMLEFT", 0, 0)
    local goBg = goBtn:CreateTexture(nil, "BACKGROUND")
    goBg:SetAllPoints(goBtn)
    goBg:SetTexture(0.15, 0.35, 0.15, 0.9)
    local goLabel = goBtn:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    goLabel:SetPoint("CENTER", goBtn, "CENTER", 0, 0)
    goLabel:SetText("|cff00ff00Set Route|r")
    goBtn:SetScript("OnClick", GoToContract)
    content.goBtn = goBtn

    local rerollBtn = CreateFrame("Button", nil, content)
    rerollBtn:SetWidth(halfW)
    rerollBtn:SetHeight(22)
    rerollBtn:SetPoint("BOTTOMRIGHT", content, "BOTTOMRIGHT", 0, 0)
    local rerollBg = rerollBtn:CreateTexture(nil, "BACKGROUND")
    rerollBg:SetAllPoints(rerollBtn)
    rerollBg:SetTexture(0.35, 0.2, 0.1, 0.9)
    local rerollLabel = rerollBtn:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    rerollLabel:SetPoint("CENTER", rerollBtn, "CENTER", 0, 0)
    rerollLabel:SetText("|cffff9040New Contract|r")
    -- A dot-command via plain SendChatMessage - the server intercepts it
    -- BEFORE broadcasting as chat (since it starts with ".") via
    -- ChatHandler::ParseCommands, so other players never see it. The
    -- response (success/failure) and updated data arrive via the same path
    -- as always (KT_RerollContract itself sends KT_SendHudUpdate) - the
    -- button is NOT updated manually here again.
        SendChatMessage(".kthudreroll", "SAY")
    end)
    content.rerollBtn = rerollBtn

    return content
end

local function GoToWeekly()
    if not weeklyState.hasContract then
        DEFAULT_CHAT_FRAME:AddMessage("|cffa335eeKillTrackerHUD:|r no weekly contract data yet.")
        return
    end
    if not weeklyState.pinAvailable then
        DEFAULT_CHAT_FRAME:AddMessage("|cffa335eeKillTrackerHUD:|r pin unavailable - you're not in the right zone.")
        return
    end
    if not SlashCmdList or not SlashCmdList["TOMTOM_WAY"] then
        DEFAULT_CHAT_FRAME:AddMessage("|cffff0000KillTrackerHUD:|r install TomTom for pins to be set.")
        return
    end
    SlashCmdList["TOMTOM_WAY"](string.format("%.2f %.2f %s", weeklyState.x or 0, weeklyState.y or 0, weeklyState.name or "?"))
end

local function BuildWeeklyTab(parent)
    local content = CreateFrame("Frame", nil, parent)
    content:SetAllPoints(parent)

    local text = content:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    text:SetPoint("TOPLEFT", content, "TOPLEFT", 4, 0)
    text:SetJustifyH("LEFT")
    text:SetWidth(PANEL_W - 20)
    text:SetHeight(80)
    content.text = text

    local countdown = content:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    countdown:SetPoint("TOPLEFT", content, "TOPLEFT", 4, -84)
    countdown:SetJustifyH("LEFT")
    countdown:SetWidth(PANEL_W - 20)
    content.countdown = countdown

    local goBtn = CreateFrame("Button", nil, content)
    goBtn:SetWidth(PANEL_W - 20)
    goBtn:SetHeight(22)
    goBtn:SetPoint("BOTTOM", content, "BOTTOM", 0, 0)
    local goBg = goBtn:CreateTexture(nil, "BACKGROUND")
    goBg:SetAllPoints(goBtn)
    goBg:SetTexture(0.35, 0.15, 0.35, 0.9)
    local goLabel = goBtn:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    goLabel:SetPoint("CENTER", goBtn, "CENTER", 0, 0)
    goLabel:SetText("|cffa335eeSet Route|r")
    goBtn:SetScript("OnClick", GoToWeekly)
    content.goBtn = goBtn

    return content
end

local function BuildUI()
    if frame then return end -- guard against being called twice

    frame = CreateFrame("Frame", "KillTrackerHUDFrame", UIParent)
    frame:SetWidth(PANEL_W)
    frame:SetHeight(PANEL_H)
    frame:SetPoint("TOPRIGHT", UIParent, "TOPRIGHT", -30, -220)
    frame:SetMovable(true)
    frame:SetClampedToScreen(true)
    frame:EnableMouse(true)
    frame:RegisterForDrag("LeftButton")
    frame:SetScript("OnDragStart", function(self) self:StartMoving() end)
    frame:SetScript("OnDragStop", function(self)
        self:StopMovingOrSizing()
        local point, _, relPoint, x, y = self:GetPoint()
        KillTrackerHUD_DB.point = point
        KillTrackerHUD_DB.relPoint = relPoint
        KillTrackerHUD_DB.x = x
        KillTrackerHUD_DB.y = y
    end)

    frame:SetBackdrop({
        bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background",
        edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
        tile = true, tileSize = 32, edgeSize = 24,
        insets = { left = 6, right = 6, top = 6, bottom = 6 },
    })
    frame:SetBackdropColor(0, 0, 0, 0.75)

    -- Overlay for the brief flash (new tier/daily contract completed) -
    -- ADD blending instead of a solid color, so it doesn't hide the text
    -- underneath, just "lights up" the panel briefly. Controlled from
    -- TriggerFlash (earlier in the file) and fades out in the OnUpdate below
    -- (frame:SetScript("OnUpdate", ...)).
    flashOverlay:SetAllPoints(frame)
    flashOverlay:SetTexture(1, 1, 1, 1)
    flashOverlay:SetBlendMode("ADD")
    flashOverlay:Hide()

    local title = frame:CreateFontString(nil, "OVERLAY", "GameFontNormal")
    title:SetPoint("TOP", frame, "TOP", 0, -8)
    title:SetText("|cffffd700Hunt Chronicler|r")

    local closeBtn = CreateFrame("Button", nil, frame)
    closeBtn:SetWidth(20)
    closeBtn:SetHeight(20)
    closeBtn:SetPoint("TOPRIGHT", frame, "TOPRIGHT", -2, -2)
    local closeText = closeBtn:CreateFontString(nil, "OVERLAY", "GameFontNormal")
    closeText:SetPoint("CENTER", closeBtn, "CENTER", 0, 0)
    closeText:SetText("|cffff5555x|r")
    closeBtn:SetScript("OnClick", function() frame:Hide() end)
    closeBtn:SetScript("OnEnter", function() closeText:SetText("|cffff0000x|r") end)
    closeBtn:SetScript("OnLeave", function() closeText:SetText("|cffff5555x|r") end)

    -- Tabs (back to 4 - [Bonuses] added; Leaderboard stays in the NPC
    -- gossip menu only). Narrower buttons than when there were 3.
    tabButtons = {}
    tabButtons[1] = CreateTabButton(frame, 1, "Stats", 8, 74)
    tabButtons[2] = CreateTabButton(frame, 2, "Contract", 84, 74)
    tabButtons[3] = CreateTabButton(frame, 3, "Weekly", 160, 74)
    tabButtons[4] = CreateTabButton(frame, 4, "Bonuses", 236, 74)

    local contentArea = CreateFrame("Frame", nil, frame)
    contentArea:SetPoint("TOPLEFT", frame, "TOPLEFT", 10, -52)
    contentArea:SetPoint("BOTTOMRIGHT", frame, "BOTTOMRIGHT", -10, 10)

    tabContents = {}
    tabContents[1] = BuildStatsTab(contentArea)
    tabContents[2] = BuildContractTab(contentArea)
    tabContents[3] = BuildWeeklyTab(contentArea)
    tabContents[4] = BuildBonusTab(contentArea)

    SetActiveTab(KillTrackerHUD_DB.activeTab or 1)

    -- Live countdown to the contract reset - computed LOCALLY between
    -- server updates (the server only sends data on login/kill, not every
    -- second). Throttled to ~once a second, to avoid extra work every
    -- frame. All wrapped in pcall - even if an error ever happens here, it
    -- won't break the rest of the addon or the game.
    frame:SetScript("OnUpdate", function(self, elapsed)
        -- Flash fade-out - OUTSIDE the throttle below (every frame, for smoothness).
        if flashOverlay and flashOverlay:IsShown() then
            flashElapsed = flashElapsed + elapsed
            local alpha = 0.55 * (1 - flashElapsed / FLASH_DURATION)
            if alpha <= 0 then
                flashOverlay:Hide()
            else
                flashOverlay:SetAlpha(alpha)
            end
        end

        countdownTicker = countdownTicker + elapsed
        if countdownTicker < 1 then return end
        countdownTicker = 0

        local ok = pcall(function()
            if contractState.hasContract and contractState.resetSecondsAtReceive
                and tabContents and tabContents[2] and tabContents[2].countdown then
                local now = GetTime and GetTime() or contractState.receivedAtClientTime
                local elapsedSince = now - (contractState.receivedAtClientTime or now)
                local remaining = math.max(0, contractState.resetSecondsAtReceive - elapsedSince)

                if contractState.completed then
                    tabContents[2].countdown:SetText(string.format("|cff888888New contract in:|r %s", FormatCountdown(remaining)))
                else
                    tabContents[2].countdown:SetText(string.format("|cff888888Until reset (regardless of completion):|r %s", FormatCountdown(remaining)))
                end
            end

            if weeklyState.hasContract and weeklyState.resetSecondsAtReceive
                and tabContents and tabContents[3] and tabContents[3].countdown then
                local now = GetTime and GetTime() or weeklyState.receivedAtClientTime
                local elapsedSince = now - (weeklyState.receivedAtClientTime or now)
                local remaining = math.max(0, weeklyState.resetSecondsAtReceive - elapsedSince)

                if weeklyState.completed then
                    tabContents[3].countdown:SetText(string.format("|cff888888New weekly contract in:|r %s", FormatCountdown(remaining)))
                else
                    tabContents[3].countdown:SetText(string.format("|cff888888Until reset (regardless of completion):|r %s", FormatCountdown(remaining)))
                end
            end
        end)
        if not ok then
            self:SetScript("OnUpdate", nil) -- don't spam errors every frame - just disable the ticker
        end
    end)

    if KillTrackerHUD_DB.point then
        frame:ClearAllPoints()
        frame:SetPoint(KillTrackerHUD_DB.point, UIParent, KillTrackerHUD_DB.relPoint or KillTrackerHUD_DB.point,
            KillTrackerHUD_DB.x or 0, KillTrackerHUD_DB.y or 0)
    end
    if KillTrackerHUD_DB.hidden then
        frame:Hide()
    end
end

local function UpdateStats(body)
    if not statLines then return end

    -- Format (version 9): version|total|tier|tierMax|remainToNextTier|streak|
    -- bestStreak|dailyKills|dailyNorm|dailyCompletions|uniqueBosses|
    -- bossBonusPct|eliteKills|eliteBonusPct|eliteRemainToNextTier|
    -- questCompleted|questTier|questRemainToNextTier|dungeonsCleared|
    -- dungeonBonusPct|allStatsPct|hitPct|spellHitPct|xpBonusPct|
    -- killTierSummary (text)|skillBonusPoints (number)|
    -- passiveNames (text)|tierProgressPct (number, 0-100)|
    -- eliteProgressPct (number, 0-100 - NEW in version 9)|
    -- questProgressPct (number, 0-100 - NEW in version 9)
    local parts = { strsplit("|", body) }

    local version = tonumber(parts[1])
    if version ~= 9 then return end -- old/foreign protocol version - ignore, don't break

    local total              = tonumber(parts[2]) or 0
    local tier                = tonumber(parts[3]) or 0
    local tierMax              = tonumber(parts[4]) or 0
    local remainToNextTier      = tonumber(parts[5]) or 0
    local streak                 = tonumber(parts[6]) or 0
    local bestStreak               = tonumber(parts[7]) or 0
    local dailyKills                = tonumber(parts[8]) or 0
    local dailyNorm                  = tonumber(parts[9]) or 0
    local dailyCompletions            = tonumber(parts[10]) or 0
    local uniqueBosses                 = tonumber(parts[11]) or 0
    local bossBonusPct                  = tonumber(parts[12]) or 0
    local eliteKills                     = tonumber(parts[13]) or 0
    local eliteBonusPct                   = tonumber(parts[14]) or 0
    local eliteRemainToNextTier            = tonumber(parts[15]) or 0
    local questCompleted                    = tonumber(parts[16]) or 0
    local questTier                          = tonumber(parts[17]) or 0
    local questRemainToNextTier               = tonumber(parts[18]) or 0
    local dungeonsCleared                     = tonumber(parts[19]) or 0
    local dungeonBonusPct                      = tonumber(parts[20]) or 0
    local allStatsPct                          = tonumber(parts[21]) or 0
    local hitPct                               = tonumber(parts[22]) or 0
    local spellHitPct                          = tonumber(parts[23]) or 0
    local xpBonusPct                           = tonumber(parts[24]) or 0
    local killTierSummary                      = parts[25] or ""
    local skillBonusPoints                     = tonumber(parts[26]) or 0
    local passiveNames                         = parts[27] or ""
    local tierProgressPct                      = tonumber(parts[28]) or 0
    local eliteProgressPct                     = tonumber(parts[29]) or 0
    local questProgressPct                     = tonumber(parts[30]) or 0

    -- Daily quota - the server doesn't send a separate progress field for
    -- it, so it's computed locally from the already-available dailyKills/dailyNorm (0-100%).
    local dailyProgressPct = dailyNorm > 0 and math.min(100, math.floor(dailyKills / dailyNorm * 100)) or 0

    statLines[1]:SetText(string.format("|cff00ff00Total killed:|r %d (tier %d/%d)", total, tier, tierMax))

    -- Flash + sound on a NEW tier (lastSeenTier == nil - first data
    -- received this session, doesn't trigger, just remembers).
    if lastSeenTier ~= nil and tier > lastSeenTier then
        TriggerFlash(1.0, 0.84, 0, "Sound\\interface\\LevelUp.wav")
    end
    lastSeenTier = tier

    if tier >= tierMax then
        statLines[2]:SetText("|cff00ccffTier:|r max")
    else
        statLines[2]:SetText(string.format("|cff00ccffNext tier in:|r %d more", remainToNextTier))
    end

    -- Tier progress bar - a visual alternative to the text above (0-100%,
    -- computed server-side from KT_MILESTONE_STEP - kill_milestones.cpp).
    if tabContents and tabContents[1] and tabContents[1].tierBar then
        tabContents[1].tierBar:SetValue(tierProgressPct)
        if tier >= tierMax then
            tabContents[1].tierBar:SetStatusBarColor(1.0, 0.84, 0) -- gold - max tier
        else
            tabContents[1].tierBar:SetStatusBarColor(0.2, 0.6, 1.0)
        end
        tabContents[1].tierBarText:SetText(string.format("%d%%", tierProgressPct))
    end

    statLines[3]:SetText(string.format("|cffffcc00Streak:|r %d (record %d)", streak, bestStreak))
    statLines[4]:SetText(string.format("|cff888888Daily quota:|r %d/%d (%d done)", dailyKills, dailyNorm, dailyCompletions))

    -- Flash + sound on daily quota completion (dailyCompletions increases
    -- by 1 each time the player reaches the quota - same approach as the
    -- daily contract).
    if lastSeenDailyCompletions ~= nil and dailyCompletions > lastSeenDailyCompletions then
        TriggerFlash(0, 1.0, 0.3, "Sound\\interface\\RaidWarning.wav")
    end
    lastSeenDailyCompletions = dailyCompletions

    -- Daily quota progress bar.
    if tabContents and tabContents[1] and tabContents[1].dailyBar then
        tabContents[1].dailyBar:SetValue(dailyProgressPct)
        if dailyProgressPct >= 100 then
            tabContents[1].dailyBar:SetStatusBarColor(0, 1.0, 0.3) -- completed for today
        else
            tabContents[1].dailyBar:SetStatusBarColor(0.5, 0.5, 0.5)
        end
        tabContents[1].dailyBarText:SetText(string.format("%d%%", dailyProgressPct))
    end

    statLines[5]:SetText(string.format("|cffff4040Bosses:|r %d (+%d%%)", uniqueBosses, bossBonusPct))

    if eliteRemainToNextTier > 0 then
        statLines[6]:SetText(string.format("|cffff8000Elite/rare:|r %d (+%d%%, %d more)", eliteKills, eliteBonusPct, eliteRemainToNextTier))
    else
        statLines[6]:SetText(string.format("|cffff8000Elite/rare:|r %d (+%d%%, max)", eliteKills, eliteBonusPct))
    end

    -- Flash + sound on a new elite/rare tier (the tier is inferred
    -- indirectly - from eliteBonusPct increasing, there's no separate
    -- numeric tier field in the protocol, and the % only grows when the tier does).
    if lastSeenEliteBonusPct ~= nil and eliteBonusPct > lastSeenEliteBonusPct then
        TriggerFlash(1.0, 0.5, 0, "Sound\\interface\\LevelUp.wav")
    end
    lastSeenEliteBonusPct = eliteBonusPct

    -- Elite/rare progress bar.
    if tabContents and tabContents[1] and tabContents[1].eliteBar then
        tabContents[1].eliteBar:SetValue(eliteProgressPct)
        if eliteRemainToNextTier == 0 then
            tabContents[1].eliteBar:SetStatusBarColor(1.0, 0.84, 0) -- gold - max tier
        else
            tabContents[1].eliteBar:SetStatusBarColor(1.0, 0.5, 0.0)
        end
        tabContents[1].eliteBarText:SetText(string.format("%d%%", eliteProgressPct))
    end

    if questRemainToNextTier > 0 then
        statLines[7]:SetText(string.format("|cff66ccffQuests:|r %d (tier %d, %d more)", questCompleted, questTier, questRemainToNextTier))
    else
        statLines[7]:SetText(string.format("|cff66ccffQuests:|r %d (tier max)", questCompleted))
    end

    -- Flash + sound on a new quest tier.
    if lastSeenQuestTier ~= nil and questTier > lastSeenQuestTier then
        TriggerFlash(0.4, 0.8, 1.0, "Sound\\interface\\LevelUp.wav")
    end
    lastSeenQuestTier = questTier

    -- Quest progress bar.
    if tabContents and tabContents[1] and tabContents[1].questBar then
        tabContents[1].questBar:SetValue(questProgressPct)
        if questRemainToNextTier == 0 then
            tabContents[1].questBar:SetStatusBarColor(1.0, 0.84, 0) -- gold - max tier
        else
            tabContents[1].questBar:SetStatusBarColor(0.4, 0.8, 1.0)
        end
        tabContents[1].questBarText:SetText(string.format("%d%%", questProgressPct))
    end

    statLines[8]:SetText(string.format("|cff40c0ffDungeons cleared:|r %d (+%d%%)", dungeonsCleared, dungeonBonusPct))

    bonusState.allStatsPct      = allStatsPct
    bonusState.hitPct           = hitPct
    bonusState.spellHitPct      = spellHitPct
    bonusState.xpBonusPct       = xpBonusPct
    bonusState.killTierSummary  = killTierSummary
    bonusState.skillBonusPoints = skillBonusPoints
    bonusState.passiveNames     = passiveNames

    if bonusLines then
        bonusLines[1]:SetText(string.format("|cffffd700All stats:|r +%d%%", allStatsPct))
        bonusLines[2]:SetText(string.format("|cff40c0ffHit chance (melee/ranged):|r +%d%%", hitPct))
        bonusLines[3]:SetText(string.format("|cffa335eeSpell hit chance:|r +%d%%", spellHitPct))
        bonusLines[4]:SetText(string.format("|cff00ff00XP bonus:|r +%d%%", xpBonusPct))
        -- Line 5 - "Milestone" (the kill-tracker's own kill tier) - armor/
        -- attack power (flat, NOT %) + attack speed (%), was already
        -- computed and granted before, but never showed up on this tab.
        if killTierSummary ~= "" then
            bonusLines[5]:SetText(string.format("|cff40ff40Milestone:|r %s", killTierSummary))
        else
            bonusLines[5]:SetText("|cff40ff40Milestone:|r no tier reached yet (400 kills)")
        end
        -- Line 6 - skills/professions/weapons (+1 to EACH stat per 10
        -- points of proficiency in any skill) - per-skill breakdown lives
        -- in the NPC's [Skills & Professions] menu.
        bonusLines[6]:SetText(string.format("|cff00ccffSkills/professions:|r +%d to each stat", skillBonusPoints))
        -- Line 7 - "Master's Gift" (the multi-trainer NPC's tier system for
        -- 20/40/60 learned abilities), ready-made text with the actual
        -- NUMBERS for specific stats (attack power, crit, speed, hit
        -- chance, dodge, health, armor), as sent by the server.
        if passiveNames ~= "" then
            bonusLines[7]:SetText(string.format("|cffff9040Master's Gift:|r %s", passiveNames))
        else
            bonusLines[7]:SetText("|cffff9040Master's Gift:|r no tier reached yet (20 abilities learned)")
        end
    end
end

local function UpdateContract(body)
    if not tabContents or not tabContents[2] then return end

    -- Format (version 2): version|hasContract(0/1)|completed(0/1)|name|
    -- location|X|Y|rewardXP|secondsUntilReset
    local parts = { strsplit("|", body) }

    local version = tonumber(parts[1])
    if version ~= 2 then return end

    contractState.hasContract  = parts[2] == "1"
    local newCompleted         = parts[3] == "1"

    -- Flash + sound on the daily contract JUST being completed (a
    -- false->true transition between two updates this session;
    -- lastSeenContractCompleted == nil - first data received, just remembers, no flash).
    if lastSeenContractCompleted == false and newCompleted == true then
        TriggerFlash(0.2, 1.0, 0.2, "Sound\\interface\\RaidWarning.wav")
    end
    lastSeenContractCompleted = newCompleted

    contractState.completed    = newCompleted
    contractState.name         = parts[4] or "?"
    contractState.area         = parts[5] or "?"
    contractState.x            = tonumber(parts[6]) or 0
    contractState.y            = tonumber(parts[7]) or 0
    contractState.rewardXp     = tonumber(parts[8]) or 0
    -- The seconds-until-reset value received FROM THE SERVER is tied to a
    -- local GetTime() snapshot at receive time - the countdown is then
    -- computed LOCALLY (every second, see countdownTicker in OnUpdate),
    -- not by waiting for a new message.
    contractState.receivedAtClientTime  = GetTime and GetTime() or 0

    local content = tabContents[2]
    if not contractState.hasContract then
        content.text:SetText("|cff888888No daily contract generated yet - visit the NPC or kill something.|r")
        content.countdown:SetText("")
        content.goBtn:Hide()
        content.rerollBtn:Hide()
    elseif contractState.completed then
        content.text:SetText(string.format(
            "|cff00ff00Complete!|r\nTarget: |cffffcc00%s|r\nLocation: %s\nReward: %d XP + %d gold",
            contractState.name, contractState.area, contractState.rewardXp, CONTRACT_GOLD_REWARD))
        content.countdown:SetText(string.format("|cff888888New contract in:|r %s", FormatCountdown(contractState.resetSecondsAtReceive)))
        content.goBtn:Hide()
        -- A completed contract can't be rerolled (so the earned reward
        -- can't be "washed away") - the same protection as on the server
        -- (KT_RerollContract returns false); the button is just hidden here.
        content.rerollBtn:Hide()
    else
        content.text:SetText(string.format(
            "Target: |cffff8000%s|r\nLocation: %s\nReward: %d XP + %d gold",
            contractState.name, contractState.area, contractState.rewardXp, CONTRACT_GOLD_REWARD))
        content.countdown:SetText(string.format("|cff888888Until reset (regardless of completion):|r %s", FormatCountdown(contractState.resetSecondsAtReceive)))
        content.goBtn:Show()
        content.rerollBtn:Show()
    end
end

local function UpdateWeekly(body)
    if not tabContents or not tabContents[3] then return end

    -- Format (version 1): version|hasContract(0/1)|completed(0/1)|name|
    -- continent|location|isDungeon(0/1)|pinAvailable(0/1)|X|Y|rewardXP|
    -- secondsUntilReset
    local parts = { strsplit("|", body) }

    local version = tonumber(parts[1])
    if version ~= 1 then return end

    weeklyState.hasContract  = parts[2] == "1"
    weeklyState.completed    = parts[3] == "1"
    weeklyState.name         = parts[4] or "?"
    weeklyState.continent    = parts[5] or "?"
    weeklyState.location     = parts[6] or "?"
    weeklyState.isDungeon    = parts[7] == "1"
    weeklyState.pinAvailable = parts[8] == "1"
    weeklyState.x            = tonumber(parts[9]) or 0
    weeklyState.y            = tonumber(parts[10]) or 0
    weeklyState.rewardXp     = tonumber(parts[11]) or 0
    weeklyState.resetSecondsAtReceive = tonumber(parts[12]) or 0
    weeklyState.receivedAtClientTime  = GetTime and GetTime() or 0

    local content = tabContents[3]
    if not weeklyState.hasContract then
        content.text:SetText("|cff888888No weekly contract generated yet - visit the NPC or kill something.|r")
        content.countdown:SetText("")
        content.goBtn:Hide()
    elseif weeklyState.completed then
        content.text:SetText(string.format(
            "|cff00ff00Complete!|r\nTarget: |cffffcc00%s|r\nLocation: %s - %s\nReward: %d XP + %d gold",
            weeklyState.name, weeklyState.continent, weeklyState.location, weeklyState.rewardXp, WEEKLY_GOLD_REWARD))
        content.countdown:SetText(string.format("|cff888888New weekly contract in:|r %s", FormatCountdown(weeklyState.resetSecondsAtReceive)))
        content.goBtn:Hide()
    else
        local locLine
        if weeklyState.pinAvailable then
            locLine = string.format("Location: %s - %s", weeklyState.continent, weeklyState.location)
        elseif weeklyState.isDungeon then
            locLine = string.format("|cff888888Boss in dungeon:|r %s (go there or to the entrance)", weeklyState.location)
        else
            locLine = string.format("|cff888888Wrong zone:|r go to %s - %s", weeklyState.continent, weeklyState.location)
        end

        content.text:SetText(string.format(
            "Target: |cffa335ee%s|r\n%s\nReward: %d XP + %d gold",
            weeklyState.name, locLine, weeklyState.rewardXp, WEEKLY_GOLD_REWARD))
        content.countdown:SetText(string.format("|cff888888Until reset (regardless of completion):|r %s", FormatCountdown(weeklyState.resetSecondsAtReceive)))

        if weeklyState.pinAvailable then
            content.goBtn:Show()
        else
            content.goBtn:Hide()
        end
    end
end

-- "Rate Holiday" - a small separate banner at the top of the screen, NOT
-- tied to the main panel (Stats/Contract/etc) - works even if the panel is
-- closed/hidden via hotkey. Doesn't stay on screen for the full 24-hour
-- holiday - only draws attention for ~60 sec (pulsing) at the holiday's
-- START and on every login WHILE the holiday is already active, then
-- smoothly fades and hides itself - so it doesn't get in the way all day.
-- The chat message (server-sent) remains the permanent record confirming
-- the start/end.
local RH_BANNER_VISIBLE_SECONDS = 60 -- how long it stays at full visibility
local RH_BANNER_FADE_SECONDS    = 3  -- how many seconds it takes to fade out at the end

local function EnsureHolidayBanner()
    if rhBanner then return rhBanner end

    rhBanner = CreateFrame("Frame", "KillTrackerHUD_HolidayBanner", UIParent)
    rhBanner:SetWidth(420)
    rhBanner:SetHeight(26)
    rhBanner:SetPoint("TOP", UIParent, "TOP", 0, -16)
    rhBanner:SetFrameStrata("HIGH")

    local text = rhBanner:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
    text:SetPoint("CENTER", rhBanner, "CENTER", 0, 0)
    text:SetText("|cffffd700*** RATE HOLIDAY! XP/gold/reputation/drops x2 ***|r")
    rhBanner.text = text
    rhBanner.pulseElapsed = 0

    rhBanner:SetScript("OnUpdate", function(self, elapsed)
        self.pulseElapsed = self.pulseElapsed + elapsed

        if self.pulseElapsed >= RH_BANNER_VISIBLE_SECONDS + RH_BANNER_FADE_SECONDS then
            self:Hide()
            return
        elseif self.pulseElapsed >= RH_BANNER_VISIBLE_SECONDS then
            -- Fade-out phase - linear from full alpha to 0.
            local fadeProgress = (self.pulseElapsed - RH_BANNER_VISIBLE_SECONDS) / RH_BANNER_FADE_SECONDS
            self.text:SetAlpha(1 - fadeProgress)
        else
            -- Pulse phase (first RH_BANNER_VISIBLE_SECONDS seconds).
            local a = 0.55 + 0.45 * math.abs(math.sin(self.pulseElapsed * 1.4))
            self.text:SetAlpha(a)
        end
    end)

    rhBanner:Hide()
    return rhBanner
end

local function ShowHolidayBanner()
    local b = EnsureHolidayBanner()
    b.pulseElapsed = 0
    b:Show()
end

local function HideHolidayBanner()
    if rhBanner then rhBanner:Hide() end
end

-- Shared parsing logic - called both from the chat filter (to hide the
-- line immediately) and DIRECTLY from the CHAT_MSG_SYSTEM event (as a
-- reliable fallback in case the filter doesn't fire for some reason - the
-- same proven mechanism already used reliably by KillTrackerWaypoint).
-- Idempotent: calling it again with the same data just overwrites the same fields.
local function ProcessMarkerMessage(msg)
    if type(msg) ~= "string" then return false end

    local handled = false
    local ok, err = pcall(function()
        if msg:sub(1, #MARKER_STATS) == MARKER_STATS then
            UpdateStats(msg:sub(#MARKER_STATS + 1))
            handled = true
        elseif msg:sub(1, #MARKER_CONTRACT) == MARKER_CONTRACT then
            UpdateContract(msg:sub(#MARKER_CONTRACT + 1))
            handled = true
        elseif msg:sub(1, #MARKER_WEEKLY) == MARKER_WEEKLY then
            UpdateWeekly(msg:sub(#MARKER_WEEKLY + 1))
            handled = true
        elseif msg:sub(1, #MARKER_BOARD) == MARKER_BOARD then
            handled = true -- just hide it from chat, there's no Leaderboard tab anymore
        elseif msg:sub(1, #MARKER_RH_START) == MARKER_RH_START then
            ShowHolidayBanner()
            handled = true
        elseif msg:sub(1, #MARKER_RH_STOP) == MARKER_RH_STOP then
            HideHolidayBanner()
            handled = true
        end
    end)

    if not ok and DEFAULT_CHAT_FRAME then
        DEFAULT_CHAT_FRAME:AddMessage("|cffff0000KillTrackerHUD:|r error (" .. tostring(err) .. ")")
    end

    return handled
end

-- Standard Blizzard chat message filter: return true so the message is
-- NOT shown to the player (it's just payload data).
local function HudChatFilter(chatFrame, event, msg, ...)
    return ProcessMarkerMessage(msg)
end

local eventFrame = CreateFrame("Frame")
eventFrame:RegisterEvent("PLAYER_LOGIN")
eventFrame:RegisterEvent("CHAT_MSG_SYSTEM")
eventFrame:SetScript("OnEvent", function(self, event, msg)
    if event == "CHAT_MSG_SYSTEM" then
        -- A fallback path around the filter - guarantees the data always
        -- reaches the panel, even if ChatFrame_AddMessageEventFilter fails
        -- to hide/handle the line itself for some reason.
        ProcessMarkerMessage(msg)
        return
    end

    local ok, err = pcall(function()
        BuildUI()
        if ChatFrame_AddMessageEventFilter then
            ChatFrame_AddMessageEventFilter("CHAT_MSG_SYSTEM", HudChatFilter)
        end
    end)

    if not ok and DEFAULT_CHAT_FRAME then
        DEFAULT_CHAT_FRAME:AddMessage("|cffff0000KillTrackerHUD:|r error (" .. tostring(err) .. ")")
    end
end)

SLASH_KILLTRACKERHUD1 = "/kthud"
SlashCmdList["KILLTRACKERHUD"] = function(msg)
    if not frame then return end -- UI not ready yet (no command needed before PLAYER_LOGIN)

    msg = string.lower(msg or "")
    if msg == "reset" then
        frame:ClearAllPoints()
        frame:SetPoint("TOPRIGHT", UIParent, "TOPRIGHT", -30, -220)
        KillTrackerHUD_DB.point, KillTrackerHUD_DB.relPoint, KillTrackerHUD_DB.x, KillTrackerHUD_DB.y = nil, nil, nil, nil
        DEFAULT_CHAT_FRAME:AddMessage("|cffffd700KillTrackerHUD:|r position reset.")
        return
    end

    if frame:IsShown() then
        frame:Hide()
        KillTrackerHUD_DB.hidden = true
    else
        frame:Show()
        KillTrackerHUD_DB.hidden = false
    end
end
