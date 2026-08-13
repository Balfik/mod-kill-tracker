// Custom module: "Kill Tracker" NPC
// Shows the player's personal kill statistics (total/normal/elite/boss),
// progress toward the next tier buff, top-5 most-killed creatures, the
// server leaderboard, the list of cosmetic titles (unlocked/locked shown
// right in the menu), and unique-boss-kill bonuses.
// The actual work (granting buffs/titles/gold) happens in
// kill_milestones.cpp via a PlayerScript hook on every kill.
//
// The entire INFORMATIONAL part (status/top-kills/leaderboard/boss and
// elite lists) is rendered DIRECTLY in the gossip menu (not chat) - clicking
// a page/entry just redraws the same menu and does NOT pile up duplicates in
// chat. Something is only written to chat as the RESULT of an action (e.g.
// clicking a boss prints a /way command), which is new information, not a repeat.
//
// This module deliberately does NOT add a spawn (per the user's request) -
// creature_template only defines the NPC (entry below); it can be placed on
// the map by hand with a command:
//     .npc add 601070
// while standing at the desired spot.

#include "Chat.h"
#include "Configuration/Config.h"
#include "DBCStores.h"
#include "LFGMgr.h"
#include "Player.h"
#include "ScriptedGossip.h"
#include "ScriptMgr.h"
#include "StringFormat.h"

#include "kill_milestones.h"

#include <set>

bool KillTrackerEnable;

constexpr uint32 SENDER_NAV = 1;
constexpr uint32 ACT_MAIN          = 999;
constexpr uint32 ACT_STATUS        = 1;
constexpr uint32 ACT_TOP_MOBS      = 2;
constexpr uint32 ACT_LEADERBOARD   = 3;
constexpr uint32 ACT_TITLES        = 4;
constexpr uint32 ACT_BOSS_BONUSES  = 5;
constexpr uint32 ACT_CONTRACT      = 6;
constexpr uint32 ACT_ELITE_BONUSES = 7;
constexpr uint32 ACT_WEEKLY_CONTRACT = 8;
constexpr uint32 ACT_DUNGEONS        = 9;
constexpr uint32 ACT_CONTRACT_REROLL = 10; // [Reload Contract] button in ShowContract
constexpr uint32 ACT_SKILLS          = 11; // [Skills & Professions] - the "+1/10 points" bonus (KT_GetSkillBonusEntries)
constexpr uint32 ACT_TELEPORT        = 12; // [Teleport to Dungeon] - main menu (ShowTeleportDungeons)

// Skill list pagination (same approach as ACT_ELITE_LIST_PAGE_BASE) -
// action = BASE + page number (0-based). 11000+ is the first free range
// after ACT_DUNGEON_TOGGLE_BASE (10000-10999).
constexpr uint32 ACT_SKILLS_LIST_PAGE_BASE = 11000; // 11000-11999

// Two-level [Cleared Dungeons] menu: continent -> flat list of that
// continent's dungeons/raids (each is just an informational line,
// green/red by status, with no pins/navigation - unlike
// [Boss Bonuses], no third "location/boss" level is needed here).
// Indices are computed from the ALWAYS consistently sorted KT_GetAllDungeons().
constexpr uint32 ACT_DUNGEON_CONTINENT_BASE = 3000; // 3000 + continent index (0..99), free range 3000-3999

// Manual credit toggle for a specific dungeon/wing (clicking an entry in the
// continent list) - action = BASE + GLOBAL INDEX in KT_GetAllDungeons()
// (same approach as ACT_BOSS_ITEM_BASE for bosses). Cannot encode
// creditEntry directly - that's a CREATURE entry (the final boss), values
// reach the tens of thousands and collide with other ranges
// (ACT_BOSS_ITEM_BASE etc.) - so only the stable list index is used.
// Needed for dungeons/wings the auto-tracker (instance_encounters,
// creditType=0) can't see (lastEncounterDungeon=0 in Blizzard's data) - and
// so an already-granted credit can be deliberately REMOVED (the same click
// acts as a toggle).
constexpr uint32 ACT_DUNGEON_TOGGLE_BASE = 10000; // 10000-10999, free range

// Two-level [Teleport to Dungeon] menu: continent -> flat list of that
// continent's dungeons (same approach and indexing via KT_GetAllDungeons()
// as [Cleared Dungeons] above), BUT clicking an entry does not toggle
// credit - it queues the player via LFG (sLFGMgr->JoinLfg,
// d.lfgDungeonId) - NOT a direct TeleportTo (that proved unreliable, see
// the change history in kill_milestones.h/.cpp).
constexpr uint32 ACT_TP_CONTINENT_BASE = 4000;  // 4000 + continent index (0..99), free range 4000-4999
constexpr uint32 ACT_TP_ITEM_BASE      = 12000; // 12000 + global index in KT_GetAllDungeons(), free range 12000-12999

// Rare/elite list pagination (KT_LIST_PAGE_SIZE in kill_milestones.h) -
// action = BASE + page number (0-based).
constexpr uint32 ACT_ELITE_LIST_PAGE_BASE = 2000; // 2000-2999

// Multi-level [Boss Bonuses] menu: continent -> location/dungeon -> boss.
// All indices come from the ALWAYS consistently sorted KT_GetAllBossLocations()
// (continent -> location -> name), so they stay stable between displaying the
// menu and handling a click within a single process run (data only changes on restart).
constexpr uint32 ACT_BOSS_CONTINENT_BASE = 5000; // 5000 + continent index (0..99)
constexpr uint32 ACT_BOSS_LOC_PAGE_BASE  = 5100; // 5100 + continent*100 + location page (5100-6099)
constexpr uint32 ACT_BOSS_LOCATION_BASE  = 7000; // 7000 + global location index (7000-7999)
constexpr uint32 ACT_BOSS_ITEM_BASE      = 8000; // 8000 + boss index in the flat list (8000-9999)

// The real "page size" for a menu with EXTRA entries around the list
// (headers/navigation/[Back]) - deliberately SMALLER than
// KT_LIST_PAGE_SIZE (28), so that combined with those 4-6 extra entries,
// the total item count in the menu never gets close to the hard
// GOSSIP_MAX_MENU_ITEMS=32 limit (exceeding it CRASHES the server).
constexpr uint32 KT_UI_PAGE_SIZE = 24;

// Max bosses shown per location without pagination (there's headroom for a
// header + 2 navigation buttons) - real locations have single/double-digit
// boss counts, so this cap almost never actually triggers.
constexpr uint32 KT_UI_LOCATION_BOSS_CAP = 27;

// The real WotLK level cap for display - NOT the same as
// LFGDungeonEntry::MaxLevel (which for raids/endgame content in the dbc is
// almost always 83, just "no upper bound", not a real cap).
// The displayed max is clamped to this number so it doesn't confuse the player.
constexpr uint32 KT_LEVEL_CAP_FOR_DISPLAY = 80;

// Text suffix "(lvl X-Y)"/"(lvl X)" plus a "[RAID]" tag for a dungeon's
// name in lists - self-contained color codes (opens and closes its own
// |cff.../|r), so it's safe to insert inside ANY already-colored string
// without "leaking" color outward. Shared by [Cleared Dungeons]
// (ShowDungeonContinent) and [Teleport to Dungeon]
// (ShowTeleportContinent).
static std::string FormatDungeonLevelSuffix(KTDungeonEntry const& d)
{
    std::string suffix;
    if (d.minLevel > 0)
    {
        uint32 displayMax = std::min(d.maxLevel, KT_LEVEL_CAP_FOR_DISPLAY);
        suffix += displayMax > d.minLevel
            ? Acore::StringFormat(" (lvl {}-{})", d.minLevel, displayMax)
            : Acore::StringFormat(" (lvl {})", d.minLevel);
    }
    if (d.isRaid)
        suffix += " |cffff6060[RAID]|r";
    return suffix;
}

// Gray in chat stays as before (|cff888888). In the gossip menu that same
// gray is barely readable against the dark menu background, so there it's
// twice as dark (each channel roughly /2: 88 -> 44).
constexpr char const* KT_GRAY_CHAT = "|cff888888";
constexpr char const* KT_GRAY_UI   = "|cff444444";

class npc_kill_tracker : public CreatureScript
{
public:
    npc_kill_tracker() : CreatureScript("npc_kill_tracker") { }

    static void ShowMainMenu(Player* player, Creature* creature)
    {
        player->PlayerTalkClass->ClearMenus();
        KTStats const& stats = KT_GetStats(player);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT,
            "|TInterface/ICONS/Ability_Hunter_SniperShot:24:24:-18|t[Status] (killed " + std::to_string(stats.total) + ")",
            SENDER_NAV, ACT_STATUS);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT,
            "|TInterface/ICONS/Ability_Hunter_MasterMarksman:24:24:-18|t[Top Kills]", SENDER_NAV, ACT_TOP_MOBS);
        // INV_Misc_Coin_02 instead of INV_Misc_Trophy_02 - the latter rendered
        // noticeably smaller than the rest of the menu entries (non-standard native texture size).
        AddGossipItemFor(player, GOSSIP_ICON_CHAT,
            "|TInterface/ICONS/INV_Misc_Coin_02:24:24:-18|t[Leaderboard]", SENDER_NAV, ACT_LEADERBOARD);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT,
            "|TInterface/ICONS/INV_Scroll_08:24:24:-18|t[Titles]", SENDER_NAV, ACT_TITLES);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT,
            "|TInterface/ICONS/Achievement_Boss_Onyxia:24:24:-18|t[Boss Bonuses] (" + std::to_string(stats.uniqueBossesKilled) + ")",
            SENDER_NAV, ACT_BOSS_BONUSES);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT,
            "|TInterface/ICONS/INV_Misc_Head_Dragon_01:24:24:-18|t[Rare/Elite] (" + std::to_string(stats.elite) + ")",
            SENDER_NAV, ACT_ELITE_BONUSES);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT,
            "|TInterface/ICONS/INV_Scroll_03:24:24:-18|t[Daily Contract] (today: " + std::to_string(stats.dailyKills) + "/" + std::to_string(KT_DAILY_NORM) + ")",
            SENDER_NAV, ACT_CONTRACT);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT,
            "|TInterface/ICONS/INV_Misc_Rune_01:24:24:-18|t[Weekly Contract]", SENDER_NAV, ACT_WEEKLY_CONTRACT);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT,
            "|TInterface/ICONS/INV_Misc_Map_01:24:24:-18|t[Cleared Dungeons]", SENDER_NAV, ACT_DUNGEONS);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT,
            "|TInterface/ICONS/Trade_BlackSmithing:24:24:-18|t[Skills & Professions] (+" + std::to_string(KT_GetSkillBonusPoints(player)) + " to all stats)",
            SENDER_NAV, ACT_SKILLS);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT,
            "|TInterface/ICONS/Spell_Shadow_Teleport:24:24:-18|t[Teleport to Dungeon]", SENDER_NAV, ACT_TELEPORT);
        player->PlayerTalkClass->SendGossipMenu(1, creature->GetGUID());
    }

    static void AddBackButton(Player* player)
    {
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "|TInterface/ICONS/INV_Misc_Note_01:24:24:-18|t[Back]", SENDER_NAV, ACT_MAIN);
    }

    // A (effectively) non-clickable info line in the gossip menu - clicking it
    // just returns to the same action (passed in by the caller), so nothing
    // duplicates or breaks. Replaces PSendSysMessage output, so redrawing the
    // menu (pagination/navigation) doesn't spam the chat window.
    static void AddInfoLine(Player* player, std::string const& text, uint32 selfAction)
    {
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, text, SENDER_NAV, selfAction);
    }

    // ---- Status: both in chat (as before) and in the gossip menu (gray in
    // the menu is twice as dark, to not blend into the menu background). ----
    static void ShowStatus(Player* player, Creature* creature)
    {
        player->PlayerTalkClass->ClearMenus();

        KTStats const& stats = KT_GetStats(player);
        ChatHandler chat(player->GetSession());

        std::string line1 = Acore::StringFormat(
            "|cff00ff00Total killed:|r {} (normal: {}, elite/rare: {}, bosses: {})",
            stats.total, stats.normal, stats.elite, stats.boss);
        chat.PSendSysMessage("{}", line1);
        AddInfoLine(player, line1, ACT_STATUS);

        std::string line2 = Acore::StringFormat(
            "|cffffcc00No-death streak:|r {} (record: {})", stats.streak, stats.bestStreak);
        chat.PSendSysMessage("{}", line2);
        AddInfoLine(player, line2, ACT_STATUS);

        if (stats.daysTracked > 0)
        {
            double perDay = double(stats.total) / double(stats.daysTracked);
            chat.PSendSysMessage("{}Pace:|r ~{:.1f} kills/day (over {} days)", KT_GRAY_CHAT, perDay, stats.daysTracked);
            AddInfoLine(player, Acore::StringFormat("{}Pace:|r ~{:.1f} kills/day (over {} days)", KT_GRAY_UI, perDay, stats.daysTracked), ACT_STATUS);
        }

        chat.PSendSysMessage("{}Daily quota:|r {}/{} killed today (total completions: {}, +{}% XP forever)",
            KT_GRAY_CHAT, stats.dailyKills, KT_DAILY_NORM, stats.dailyCompletions, stats.dailyCompletions);
        AddInfoLine(player, Acore::StringFormat("{}Daily quota:|r {}/{} killed today (total completions: {}, +{}% XP forever)",
            KT_GRAY_UI, stats.dailyKills, KT_DAILY_NORM, stats.dailyCompletions, stats.dailyCompletions), ACT_STATUS);

        auto topZones = KT_GetTopZones(player, 1);
        if (!topZones.empty())
        {
            chat.PSendSysMessage("{}Favorite farming zone:|r {} ({} killed)", KT_GRAY_CHAT, topZones[0].zoneName, topZones[0].kills);
            AddInfoLine(player, Acore::StringFormat("{}Favorite farming zone:|r {} ({} killed)", KT_GRAY_UI, topZones[0].zoneName, topZones[0].kills), ACT_STATUS);
        }

        uint32 eliteTier = uint32(std::min<uint32>(stats.elite / KT_ELITE_TIER_STEP, KT_ELITE_MAX_TIER));
        std::string line6 = Acore::StringFormat(
            "|cffff8000Elite/rare bonus:|r +{}% to all stats (details - [Rare/Elite])",
            eliteTier * KT_ELITE_STAT_PCT_PER_TIER);
        chat.PSendSysMessage("{}", line6);
        AddInfoLine(player, line6, ACT_STATUS);

        uint32 bossPct = std::min<uint32>(stats.uniqueBossesKilled, KT_BOSS_MAX_UNIQUE) * KT_BOSS_STAT_PCT_PER_BOSS;
        std::string line7 = Acore::StringFormat(
            "|cffff4040Boss bonus:|r +{}% to all stats (details - [Boss Bonuses])", bossPct);
        chat.PSendSysMessage("{}", line7);
        AddInfoLine(player, line7, ACT_STATUS);

        uint32 currentTier = uint32(std::min<uint32>(stats.total / KT_MILESTONE_STEP, KT_MILESTONE_MAX_TIER));
        std::string line8;
        if (currentTier >= KT_MILESTONE_MAX_TIER)
        {
            line8 = Acore::StringFormat(
                "|cff00ccffKill buff:|r max tier {} reached (+{} armor, +{} attack power, +{}% attack speed).",
                KT_MILESTONE_MAX_TIER, KT_MILESTONE_MAX_TIER * KT_ARMOR_PER_TIER,
                KT_MILESTONE_MAX_TIER * KT_ATTACK_POWER_PER_TIER, KT_MILESTONE_MAX_TIER * KT_HASTE_PCT_PER_TIER);
        }
        else
        {
            uint32 nextThreshold = (currentTier + 1) * KT_MILESTONE_STEP;
            uint32 remaining = nextThreshold - stats.total;
            line8 = Acore::StringFormat(
                "|cff00ccffKill buff:|r currently tier {} (+{} armor, +{} attack power, +{}% attack speed). "
                "{} more kill(s) to tier {}.",
                currentTier, currentTier * KT_ARMOR_PER_TIER, currentTier * KT_ATTACK_POWER_PER_TIER,
                currentTier * KT_HASTE_PCT_PER_TIER, remaining, currentTier + 1);
        }
        chat.PSendSysMessage("{}", line8);
        AddInfoLine(player, line8, ACT_STATUS);

        AddBackButton(player);
        player->PlayerTalkClass->SendGossipMenu(1, creature->GetGUID());
    }

    // ---- Top kills (now shown in the gossip menu too, not just chat) ----
    static void ShowTopMobs(Player* player, Creature* creature)
    {
        player->PlayerTalkClass->ClearMenus();

        auto top = KT_GetTopCreatures(player, 5);
        if (top.empty())
        {
            AddInfoLine(player, std::string(KT_GRAY_UI) + "You haven't killed anyone yet.|r", ACT_TOP_MOBS);
        }
        else
        {
            AddInfoLine(player, "|cff00ff00Your most frequent victims:|r", ACT_TOP_MOBS);
            uint32 place = 1;
            for (auto const& entry : top)
            {
                AddInfoLine(player, Acore::StringFormat(
                    "  {}. {} [{}] - {} killed", place++, entry.name, entry.creatureEntry, entry.kills), ACT_TOP_MOBS);
            }
        }

        AddBackButton(player);
        player->PlayerTalkClass->SendGossipMenu(1, creature->GetGUID());
    }

    // ---- Leaderboard (top-5, now shown in the gossip menu too) ----
    static void ShowLeaderboard(Player* player, Creature* creature)
    {
        player->PlayerTalkClass->ClearMenus();

        auto board = KT_GetLeaderboard(5);
        if (board.empty())
        {
            AddInfoLine(player, std::string(KT_GRAY_UI) + "The leaderboard is still empty.|r", ACT_LEADERBOARD);
        }
        else
        {
            AddInfoLine(player, "|cff00ff00Server leaderboard (total kills):|r", ACT_LEADERBOARD);
            uint32 place = 1;
            for (auto const& entry : board)
            {
                bool isMe = (entry.name == player->GetName());
                AddInfoLine(player, Acore::StringFormat(
                    "  {}. {}{}{} - {}", place++, isMe ? "|cffffd700" : "", entry.name, isMe ? "|r (you)" : "", entry.totalKills), ACT_LEADERBOARD);
            }
        }

        AddBackButton(player);
        player->PlayerTalkClass->SendGossipMenu(1, creature->GetGUID());
    }

    // Titles are now shown DIRECTLY in the gossip menu (not just chat) -
    // locked ones red, unlocked ones green, with the gold reward for
    // unlocking next to each. Clicking a title does nothing except return
    // to this same list (there's no separate action).
    static void ShowTitles(Player* player, Creature* creature)
    {
        player->PlayerTalkClass->ClearMenus();

        KTStats const& stats = KT_GetStats(player);
        uint32 currentTier = uint32(std::min<uint32>(stats.total / KT_MILESTONE_STEP, KT_MILESTONE_MAX_TIER));

        for (uint32 i = 0; i < KT_TITLE_COUNT; ++i)
        {
            uint32 tierNeeded = (i + 1) * KT_TITLE_TIER_STEP;
            uint32 goldReward = (i + 1) * 100; // KT_TITLE_GOLD_STEP (copper) = 100 gold per titleNumber
            bool unlocked = currentTier >= tierNeeded;

            std::string text = (unlocked ? std::string("|cff2ecc40") : std::string("|cff8b0000"))
                + "\"" + KT_GetTitleName(i) + "\" (tier " + std::to_string(tierNeeded) + ") - reward "
                + std::to_string(goldReward) + " gold"
                + (unlocked ? " - unlocked" : " - locked") + "|r";

            AddGossipItemFor(player, unlocked ? GOSSIP_ICON_INTERACT_1 : GOSSIP_ICON_TALK, text, SENDER_NAV, ACT_TITLES);
        }

        AddBackButton(player);
        player->PlayerTalkClass->SendGossipMenu(1, creature->GetGUID());
    }

    // ---- Rare/Elite: bonus progress stays in CHAT (as before), the
    // list itself moves to the gossip menu (no color, with IDs), pagination unchanged. ----
    static void ShowEliteBonuses(Player* player, Creature* creature, uint32 page = 0)
    {
        player->PlayerTalkClass->ClearMenus();

        KTStats const& stats = KT_GetStats(player);
        uint32 currentTier = uint32(std::min<uint32>(stats.elite / KT_ELITE_TIER_STEP, KT_ELITE_MAX_TIER));
        uint32 totalPct = currentTier * KT_ELITE_STAT_PCT_PER_TIER;

        ChatHandler(player->GetSession()).PSendSysMessage(
            "|cff00ff00Elite/rare killed:|r {} - permanent bonus |cffffd700+{}% to all stats|r",
            stats.elite, totalPct);

        if (currentTier >= KT_ELITE_MAX_TIER)
        {
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff00ccffMax tier {} reached.|r", KT_ELITE_MAX_TIER);
        }
        else
        {
            uint32 nextThreshold = (currentTier + 1) * KT_ELITE_TIER_STEP;
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff00ccff{} more kill(s) to the next tier ({}, +{}%).|r",
                nextThreshold - stats.elite, currentTier + 1, (currentTier + 1) * KT_ELITE_STAT_PCT_PER_TIER);
        }

        // List of SPECIFIC creature types the player has killed - just the
        // name, mob ID and count, WITHOUT color (plain standard menu item text).
        auto eliteRare = KT_GetEliteRareKills(player);
        if (eliteRare.empty())
        {
            AddInfoLine(player, std::string(KT_GRAY_UI) + "No elite/rare kills yet.|r", ACT_ELITE_LIST_PAGE_BASE + page);
        }
        else
        {
            uint32 totalPages = uint32((eliteRare.size() + KT_UI_PAGE_SIZE - 1) / KT_UI_PAGE_SIZE);
            if (page >= totalPages)
                page = totalPages - 1;

            uint32 startIdx = page * KT_UI_PAGE_SIZE;
            uint32 endIdx = std::min<uint32>(startIdx + KT_UI_PAGE_SIZE, uint32(eliteRare.size()));

            for (uint32 i = startIdx; i < endIdx; ++i)
            {
                KTCreatureKillEntry const& c = eliteRare[i];
                std::string text = c.name + " [" + std::to_string(c.creatureEntry) + "] - " + std::to_string(c.kills) + "x";
                AddGossipItemFor(player, GOSSIP_ICON_TALK, text, SENDER_NAV, ACT_ELITE_LIST_PAGE_BASE + page);
            }

            if (totalPages > 1)
            {
                AddInfoLine(player, Acore::StringFormat("{}Page {}/{}|r", KT_GRAY_UI, page + 1, totalPages),
                    ACT_ELITE_LIST_PAGE_BASE + page);
                if (page > 0)
                    AddGossipItemFor(player, GOSSIP_ICON_CHAT, "|TInterface/ICONS/Spell_Shadow_Teleport:24:24:-18|t[<- Previous page]", SENDER_NAV, ACT_ELITE_LIST_PAGE_BASE + page - 1);
                if (page + 1 < totalPages)
                    AddGossipItemFor(player, GOSSIP_ICON_CHAT, "|TInterface/ICONS/Spell_Shadow_Teleport:24:24:-18|t[Next page ->]", SENDER_NAV, ACT_ELITE_LIST_PAGE_BASE + page + 1);
            }
        }

        AddBackButton(player);
        player->PlayerTalkClass->SendGossipMenu(1, creature->GetGUID());
    }

    // ---- Skills & Professions: +1 to all stats for every 10 points of
    // proficiency (KT_SKILL_POINTS_PER_BONUS) - primary/secondary professions
    // and weapon/defense skills (KT_GetSkillBonusEntries filters categories).
    // Pagination follows the same approach as [Rare/Elite] (KT_UI_PAGE_SIZE) -
    // on this server the actual skill list is small (a dozen or two), so
    // pagination here is more future-proofing (more classes/skill expansions)
    // than a current hard need. ----
    static void ShowSkillBonuses(Player* player, Creature* creature, uint32 page = 0)
    {
        player->PlayerTalkClass->ClearMenus();

        auto skills = KT_GetSkillBonusEntries(player);
        uint32 totalPoints = 0;
        for (KTSkillBonusEntry const& s : skills)
            totalPoints += s.bonusPoints;

        ChatHandler(player->GetSession()).PSendSysMessage(
            "|cff00ff00Skills & Professions:|r permanent bonus |cffffd700+{} to EACH stat|r "
            "(str/agi/sta/int/spirit) - per {} points of proficiency in any skill.",
            totalPoints, KT_SKILL_POINTS_PER_BONUS);

        if (skills.empty())
        {
            AddInfoLine(player, std::string(KT_GRAY_UI) + "No leveled skills/professions.|r", ACT_SKILLS_LIST_PAGE_BASE + page);
        }
        else
        {
            uint32 totalPages = uint32((skills.size() + KT_UI_PAGE_SIZE - 1) / KT_UI_PAGE_SIZE);
            if (page >= totalPages)
                page = totalPages - 1;

            uint32 startIdx = page * KT_UI_PAGE_SIZE;
            uint32 endIdx = std::min<uint32>(startIdx + KT_UI_PAGE_SIZE, uint32(skills.size()));

            for (uint32 i = startIdx; i < endIdx; ++i)
            {
                KTSkillBonusEntry const& s = skills[i];
                std::string text = s.name + ": " + std::to_string(s.value) + "/" + std::to_string(s.maxValue)
                    + " (+" + std::to_string(s.bonusPoints) + ")";
                AddInfoLine(player, text, ACT_SKILLS_LIST_PAGE_BASE + page);
            }

            if (totalPages > 1)
            {
                AddInfoLine(player, Acore::StringFormat("{}Page {}/{}|r", KT_GRAY_UI, page + 1, totalPages),
                    ACT_SKILLS_LIST_PAGE_BASE + page);
                if (page > 0)
                    AddGossipItemFor(player, GOSSIP_ICON_CHAT, "|TInterface/ICONS/Spell_Shadow_Teleport:24:24:-18|t[<- Previous page]", SENDER_NAV, ACT_SKILLS_LIST_PAGE_BASE + page - 1);
                if (page + 1 < totalPages)
                    AddGossipItemFor(player, GOSSIP_ICON_CHAT, "|TInterface/ICONS/Spell_Shadow_Teleport:24:24:-18|t[Next page ->]", SENDER_NAV, ACT_SKILLS_LIST_PAGE_BASE + page + 1);
            }
        }

        AddBackButton(player);
        player->PlayerTalkClass->SendGossipMenu(1, creature->GetGUID());
    }

    // ---- Boss Bonuses: continent -> location/dungeon -> boss ----

    struct BossLocGroup
    {
        std::string location;
        bool isDungeon = false;
        std::vector<uint32> bossIdx; // indices in the flat allBosses list
    };
    struct BossContinentGroup
    {
        std::string continent;
        std::vector<BossLocGroup> locations;
    };

    // Groups the already-sorted (continent -> location -> name) flat list
    // from KT_GetAllBossLocations() into a continent->location tree. The
    // order is fully determined by the sort in kill_milestones.cpp - the
    // same on every call, so global location/boss indices stay stable
    // between displaying the menu and handling a click (within a process run).
    static std::vector<BossContinentGroup> BuildBossGroups(std::vector<KTBossEntry> const& allBosses)
    {
        std::vector<BossContinentGroup> groups;
        for (uint32 i = 0; i < uint32(allBosses.size()); ++i)
        {
            KTBossEntry const& b = allBosses[i];
            if (groups.empty() || groups.back().continent != b.continent)
                groups.push_back(BossContinentGroup{ b.continent, {} });

            auto& locs = groups.back().locations;
            if (locs.empty() || locs.back().location != b.location)
            {
                BossLocGroup g;
                g.location = b.location;
                g.isDungeon = b.isDungeon;
                locs.push_back(std::move(g));
            }
            locs.back().bossIdx.push_back(i);
        }
        return groups;
    }

    static uint32 GlobalLocationIndex(std::vector<BossContinentGroup> const& groups, uint32 continentIdx, uint32 localLocIdx)
    {
        uint32 idx = 0;
        for (uint32 c = 0; c < continentIdx; ++c)
            idx += uint32(groups[c].locations.size());
        return idx + localLocIdx;
    }

    // Top level - list of continents (+"Dungeons" as a separate category).
    static void ShowBossBonuses(Player* player, Creature* creature)
    {
        player->PlayerTalkClass->ClearMenus();

        KTStats const& stats = KT_GetStats(player);
        uint32 totalPct = std::min<uint32>(stats.uniqueBossesKilled, KT_BOSS_MAX_UNIQUE) * KT_BOSS_STAT_PCT_PER_BOSS;

        std::string bossHeader = Acore::StringFormat(
            "|cff00ff00Unique bosses killed:|r {} - permanent bonus |cffffd700+{}% to all stats|r",
            stats.uniqueBossesKilled, totalPct);
        ChatHandler(player->GetSession()).PSendSysMessage("{}", bossHeader);
        AddInfoLine(player, bossHeader, ACT_BOSS_BONUSES);

        auto allBosses = KT_GetAllBossLocations();
        auto groups = BuildBossGroups(allBosses);

        if (groups.empty())
        {
            AddInfoLine(player, std::string(KT_GRAY_UI) + "No spawned bosses found on the server.|r", ACT_BOSS_BONUSES);
        }
        else
        {
            for (uint32 c = 0; c < uint32(groups.size()); ++c)
            {
                uint32 bossCount = 0;
                for (auto const& loc : groups[c].locations)
                    bossCount += uint32(loc.bossIdx.size());

                AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                    groups[c].continent + " (" + std::to_string(bossCount) + ")", SENDER_NAV, ACT_BOSS_CONTINENT_BASE + c);
            }
        }

        AddBackButton(player);
        player->PlayerTalkClass->SendGossipMenu(1, creature->GetGUID());
    }

    // Second level - locations (zones/dungeons) of the chosen continent, paginated.
    static void ShowBossContinent(Player* player, Creature* creature, uint32 continentIdx, uint32 page)
    {
        player->PlayerTalkClass->ClearMenus();

        auto allBosses = KT_GetAllBossLocations();
        auto groups = BuildBossGroups(allBosses);

        if (continentIdx >= uint32(groups.size()))
        {
            ShowBossBonuses(player, creature);
            return;
        }

        BossContinentGroup const& group = groups[continentIdx];
        AddInfoLine(player, "|cffffd700" + group.continent + "|r - choose a location:", ACT_BOSS_CONTINENT_BASE + continentIdx);

        uint32 totalPages = group.locations.empty() ? 1 : uint32((group.locations.size() + KT_UI_PAGE_SIZE - 1) / KT_UI_PAGE_SIZE);
        if (page >= totalPages)
            page = totalPages - 1;

        uint32 startIdx = page * KT_UI_PAGE_SIZE;
        uint32 endIdx = std::min<uint32>(startIdx + KT_UI_PAGE_SIZE, uint32(group.locations.size()));

        // The same icon parameter (GOSSIP_ICON_CHAT, standard client size)
        // for ALL lines - previously this used DIFFERENT inline textures
        // (|T...|t) depending on dungeon/zone, which have different native
        // sizes and made the list look inconsistent in size.
        // Dungeon/zone is now distinguished by TEXT, not icon.
        for (uint32 i = startIdx; i < endIdx; ++i)
        {
            BossLocGroup const& loc = group.locations[i];
            uint32 globalLocIdx = GlobalLocationIndex(groups, continentIdx, i);
            std::string text = (loc.isDungeon ? std::string("[Dungeon] ") : std::string(""))
                + loc.location + " (" + std::to_string(loc.bossIdx.size()) + ")";
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, text, SENDER_NAV, ACT_BOSS_LOCATION_BASE + globalLocIdx);
        }

        if (totalPages > 1)
        {
            AddInfoLine(player, Acore::StringFormat("{}Page {}/{}|r", KT_GRAY_UI, page + 1, totalPages),
                ACT_BOSS_LOC_PAGE_BASE + continentIdx * 100 + page);
            if (page > 0)
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, "|TInterface/ICONS/Spell_Shadow_Teleport:24:24:-18|t[<- Previous page]", SENDER_NAV, ACT_BOSS_LOC_PAGE_BASE + continentIdx * 100 + page - 1);
            if (page + 1 < totalPages)
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, "|TInterface/ICONS/Spell_Shadow_Teleport:24:24:-18|t[Next page ->]", SENDER_NAV, ACT_BOSS_LOC_PAGE_BASE + continentIdx * 100 + page + 1);
        }

        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "|TInterface/ICONS/INV_Misc_Note_01:24:24:-18|t[<- Continents]", SENDER_NAV, ACT_BOSS_BONUSES);
        AddBackButton(player);
        player->PlayerTalkClass->SendGossipMenu(1, creature->GetGUID());
    }

    // Third level - the bosses of the chosen location themselves
    // (green=credit already granted, red=not yet), with a real entry ID and a click-pin.
    static void ShowBossLocation(Player* player, Creature* creature, uint32 globalLocIdx)
    {
        player->PlayerTalkClass->ClearMenus();

        auto allBosses = KT_GetAllBossLocations();
        auto groups = BuildBossGroups(allBosses);

        // Find the location group by global index.
        uint32 running = 0;
        BossLocGroup const* targetLoc = nullptr;
        uint32 targetContinentIdx = 0;
        for (uint32 c = 0; c < uint32(groups.size()); ++c)
        {
            if (globalLocIdx < running + uint32(groups[c].locations.size()))
            {
                targetLoc = &groups[c].locations[globalLocIdx - running];
                targetContinentIdx = c;
                break;
            }
            running += uint32(groups[c].locations.size());
        }

        if (!targetLoc)
        {
            ShowBossBonuses(player, creature);
            return;
        }

        auto credited = KT_GetCreditedBossEntries(player);
        std::set<uint32> creditedSet(credited.begin(), credited.end());

        AddInfoLine(player, "|cffffd700" + targetLoc->location + "|r" + (targetLoc->isDungeon ? " (dungeon - clicking a boss shows a pin if you're already inside or near the entrance)" : " - click a boss to show coordinates"),
            ACT_BOSS_LOCATION_BASE + globalLocIdx);

        // Safety cap (real locations have single/double-digit boss counts,
        // so this practically never triggers) - protects against exceeding
        // GOSSIP_MAX_MENU_ITEMS=32 together with the header/navigation below.
        uint32 shown = 0;
        for (uint32 idx : targetLoc->bossIdx)
        {
            if (shown >= KT_UI_LOCATION_BOSS_CAP)
            {
                AddInfoLine(player, Acore::StringFormat(
                    "{}... and {} more (list too long for one menu)|r",
                    KT_GRAY_UI, uint32(targetLoc->bossIdx.size()) - shown), ACT_BOSS_LOCATION_BASE + globalLocIdx);
                break;
            }

            KTBossEntry const& b = allBosses[idx];
            bool got = creditedSet.count(b.creatureEntry) != 0;
            std::string text = (got ? std::string("|cff2ecc40") : std::string("|cff8b0000"))
                + b.name + " [" + std::to_string(b.creatureEntry) + "] (lvl " + b.level + ")" + "|r";
            // SAME icon for all (status is only shown via text color) -
            // previously green/red used DIFFERENT stock client icons
            // (INTERACT_1/TALK), which looked like a "different style" between lines.
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, text, SENDER_NAV, ACT_BOSS_ITEM_BASE + idx);
            ++shown;
        }

        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "|TInterface/ICONS/INV_Misc_Note_01:24:24:-18|t[<- " + groups[targetContinentIdx].continent + "]", SENDER_NAV, ACT_BOSS_CONTINENT_BASE + targetContinentIdx);
        AddBackButton(player);
        player->PlayerTalkClass->SendGossipMenu(1, creature->GetGUID());
    }

    // Click on a specific boss. IMPORTANT: the text form "/way <Zone> X Y"
    // (TomTom searching all world zones by name) proved unreliable on this
    // server/client - repeatedly confirmed (Ashenvale ->
    // Northrend, Darnassus -> also wrong, differently every time) - the
    // client files apparently have ambiguous zone-name matches, and TomTom
    // lands on the first match, not necessarily the correct one. So /way is
    // only printed if the player is PHYSICALLY standing in the same
    // zone/instance as the target (the 100%-reliable "current zone" format -
    // the same one used by the daily contract). If not, we just honestly say
    // where to go, without attempting an unreliable cross-zone pin.
    static void HandleBossItemClick(Player* player, Creature* creature, uint32 bossIdx)
    {
        auto allBosses = KT_GetAllBossLocations();
        if (bossIdx >= uint32(allBosses.size()))
        {
            ShowBossBonuses(player, creature);
            return;
        }

        KTBossEntry const& b = allBosses[bossIdx];

        ChatHandler(player->GetSession()).PSendSysMessage(
            "|cffffd700Target:|r \"{}\" [{}] (lvl {}) - {}{}", b.name, b.creatureEntry, b.level, b.location,
            b.isDungeon ? " (dungeon)" : "");

        uint32 curMap = player->GetMapId();
        uint32 curZone = player->GetZoneId();

        if (b.hasOwnPin && curMap == b.ownMapId && curZone == b.ownZoneId)
        {
            // The player is ALREADY in the same zone/instance as the boss -
            // a reliable "current zone" /way, without a zone name.
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff888888TomTom:|r /way {:.2f} {:.2f} {}", b.ownPinX, b.ownPinY, b.name);
        }
        else if (b.hasEntrancePin && curMap == b.entranceMapId && curZone == b.entranceZoneId)
        {
            // The player is standing OUTSIDE, in the zone where the dungeon entrance is.
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff888888TomTom (dungeon entrance):|r /way {:.2f} {:.2f} {}", b.entrancePinX, b.entrancePinY, b.name);
        }
        else
        {
            // The player is in a DIFFERENT zone - honestly say where to go,
            // without attempting an automatic cross-zone pin (that's what used to send players the wrong way).
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff888888You're not in the right zone right now.|r Go to \"{}\"{} and click the boss again - a TomTom "
                "pin will appear automatically.",
                b.location, b.isDungeon ? " (or to the dungeon entrance)" : "");
        }

        // Return to that same location list (the stable global index is
        // recomputed fresh from the same deterministic sort).
        auto groups = BuildBossGroups(allBosses);
        uint32 running = 0;
        for (uint32 c = 0; c < uint32(groups.size()); ++c)
        {
            for (uint32 l = 0; l < uint32(groups[c].locations.size()); ++l)
            {
                for (uint32 idx : groups[c].locations[l].bossIdx)
                {
                    if (idx == bossIdx)
                    {
                        ShowBossLocation(player, creature, running + l);
                        return;
                    }
                }
            }
            running += uint32(groups[c].locations.size());
        }

        ShowBossBonuses(player, creature);
    }

    static void ShowContract(Player* player, Creature* creature)
    {
        player->PlayerTalkClass->ClearMenus();

        KTDailyContract const& contract = KT_GetOrGenerateContract(player);
        if (!contract.hasContract)
        {
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff888888Couldn't find a target in your current zone today. Try again later or in another zone.|r");
        }
        else if (contract.completed)
        {
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff2ecc40Daily contract complete!|r The target was: \"{}\" ({}). A new target appears tomorrow.",
                contract.creatureName, contract.areaName);
        }
        else
        {
            float mapX, mapY;
            KT_GetContractMapCoords(contract, mapX, mapY);

            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cffffd700Daily contract:|r find and kill \"{}\" [{}]", contract.creatureName, contract.creatureEntry);
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff888888Location:|r zone \"{}\"", contract.areaName);
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff888888TomTom:|r /way {:.2f} {:.2f} {}", mapX, mapY, contract.creatureName);
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff888888Or use .gps to see your coordinates to compare with:|r X:{:.1f} Y:{:.1f} Z:{:.1f}",
                contract.posX, contract.posY, contract.posZ);
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff00ccffReward:|r {} XP + {} gold", contract.rewardXp, KT_CONTRACT_GOLD_REWARD);
        }

        // History of ALREADY completed contracts (a separate append-only
        // table mq_contract_history - mq_daily_contract only stores "today").
        // PER THE USER'S REQUEST: gossip menu only (AddInfoLine - same
        // approach as Leaderboard/Titles), NOT chat - unlike the rest of
        // the info on this screen (target/location/reward), which stays
        // in chat as before.
        auto history = KT_GetContractHistory(player, 10);
        if (!history.empty())
        {
            AddInfoLine(player, "|cff00ff00Completed contract history:|r", ACT_CONTRACT);
            for (auto const& h : history)
            {
                AddInfoLine(player, Acore::StringFormat(
                    "  {} - \"{}\" ({}) - {} XP + {} gold.",
                    h.date, h.creatureName, h.areaName, h.rewardXp, KT_CONTRACT_GOLD_REWARD), ACT_CONTRACT);
            }
        }

        // Reload button - ONLY if the contract exists and hasn't been
        // completed today yet (so an already-earned reward can't be "washed
        // away"). For when an unreasonable target was rolled (e.g. a friendly/green mob).
        if (contract.hasContract && !contract.completed)
            AddGossipItemFor(player, GOSSIP_ICON_TALK,
                "|TInterface/ICONS/Ability_Repair:24:24:-18|t[Reload Contract]", SENDER_NAV, ACT_CONTRACT_REROLL);

        AddBackButton(player);
        player->PlayerTalkClass->SendGossipMenu(1, creature->GetGUID());
    }

    // Weekly bonus contract: target/location/reward/pin - in chat (same
    // format as the daily contract above). The pin is only shown if the
    // player is physically in the right zone/instance - exactly the same
    // zone check as HandleBossItemClick for regular bosses (own-pin/entrance-pin). If
    // the target is in a dungeon and the player isn't there, the dungeon
    // name is honestly printed, without attempting to show a pin. Completed
    // history stays gossip-menu only (AddInfoLine), same as the daily contract.
    static void ShowWeeklyContract(Player* player, Creature* creature)
    {
        player->PlayerTalkClass->ClearMenus();

        KTWeeklyContract const& contract = KT_GetOrGenerateWeeklyContract(player);
        if (!contract.hasContract)
        {
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff888888Couldn't pick a target this week (the boss list is empty). Try again later.|r");
        }
        else if (contract.completed)
        {
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff2ecc40Weekly contract complete!|r The target was: \"{}\" ({}). A new target appears next week.",
                contract.creatureName, contract.location);
        }
        else
        {
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cffa335eeWeekly contract:|r find and kill \"{}\" [{}] (lvl {}{})",
                contract.creatureName, contract.creatureEntry, contract.location,
                contract.isDungeon ? ", dungeon" : "");
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff888888Location:|r {} - {}", contract.continent, contract.location);

            uint32 curMap = player->GetMapId();
            uint32 curZone = player->GetZoneId();

            if (contract.hasOwnPin && curMap == contract.ownMapId && curZone == contract.ownZoneId)
            {
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cff888888TomTom:|r /way {:.2f} {:.2f} {}", contract.ownPinX, contract.ownPinY, contract.creatureName);
            }
            else if (contract.hasEntrancePin && curMap == contract.entranceMapId && curZone == contract.entranceZoneId)
            {
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cff888888TomTom (dungeon entrance):|r /way {:.2f} {:.2f} {}",
                    contract.entrancePinX, contract.entrancePinY, contract.creatureName);
            }
            else if (contract.isDungeon)
            {
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cff888888Boss in dungeon \"{}\"|r - go there (or stand at the entrance) to see the TomTom pin.",
                    contract.location);
            }
            else
            {
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cff888888You're not in the right zone right now.|r Go to \"{}\" and reopen this menu - a TomTom "
                    "pin will appear automatically.", contract.location);
            }

            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff00ccffReward:|r {} XP + {} gold", contract.rewardXp, KT_WEEKLY_GOLD_REWARD);
        }

        auto history = KT_GetWeeklyContractHistory(player, 10);
        if (!history.empty())
        {
            AddInfoLine(player, "|cff00ff00Completed weekly contract history:|r", ACT_WEEKLY_CONTRACT);
            for (auto const& h : history)
            {
                AddInfoLine(player, Acore::StringFormat(
                    "  {} - \"{}\" ({}) - {} XP + {} gold.",
                    h.date, h.creatureName, h.areaName, h.rewardXp, KT_WEEKLY_GOLD_REWARD), ACT_WEEKLY_CONTRACT);
            }
        }

        AddBackButton(player);
        player->PlayerTalkClass->SendGossipMenu(1, creature->GetGUID());
    }

    // ---- Cleared dungeons/raids ----
    // Top level - list of continents (Eastern Kingdoms/Kalimdor/
    // Outland/Northrend/Other), each with a dungeon count. Grouped from the
    // ALREADY-sorted KT_GetAllDungeons() (continent -> name), no separate
    // sort here - the same principle as [Boss Bonuses].
    static void ShowDungeons(Player* player, Creature* creature)
    {
        player->PlayerTalkClass->ClearMenus();

        auto dungeons = KT_GetAllDungeons();
        auto credited = KT_GetCreditedDungeonEntries(player);
        std::set<uint32> creditedSet(credited.begin(), credited.end());

        uint32 totalPct = std::min<uint32>(uint32(creditedSet.size()), KT_DUNGEON_MAX_UNIQUE) * KT_DUNGEON_STAT_PCT_PER_DUNGEON;
        std::string header = Acore::StringFormat(
            "|cff00ff00Dungeons cleared:|r {} of {} - permanent bonus |cffffd700+{}% to all stats|r",
            creditedSet.size(), dungeons.size(), totalPct);
        ChatHandler(player->GetSession()).PSendSysMessage("{}", header);
        AddInfoLine(player, header, ACT_DUNGEONS);

        if (dungeons.empty())
        {
            AddInfoLine(player, std::string(KT_GRAY_UI) + "No known dungeons found on the server.|r", ACT_DUNGEONS);
        }
        else
        {
            std::vector<std::pair<std::string, uint32>> continents; // name, count
            for (auto const& d : dungeons)
            {
                if (continents.empty() || continents.back().first != d.continent)
                    continents.push_back({ d.continent, 0 });
                ++continents.back().second;
            }

            for (uint32 c = 0; c < uint32(continents.size()); ++c)
                AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                    continents[c].first + " (" + std::to_string(continents[c].second) + ")",
                    SENDER_NAV, ACT_DUNGEON_CONTINENT_BASE + c);
        }

        AddBackButton(player);
        player->PlayerTalkClass->SendGossipMenu(1, creature->GetGUID());
    }

    // Continent index (0-based, in the SAME order as the grouping in
    // ShowDungeons/ShowDungeonContinent) for a dungeon/wing with a given
    // creditEntry - used after a toggle, to return the player to the same
    // continent page they clicked from.
    static uint32 FindContinentIndexForDungeon(std::vector<KTDungeonEntry> const& dungeons, uint32 creditEntry)
    {
        std::string lastContinent;
        uint32 idx = uint32(-1);
        for (auto const& d : dungeons)
        {
            if (lastContinent.empty() || lastContinent != d.continent)
            {
                ++idx;
                lastContinent = d.continent;
            }
            if (d.creditEntry == creditEntry)
                return idx;
        }
        return 0;
    }

    // Second level - flat list of the chosen continent's dungeons. Every
    // entry is CLICKABLE (toggles credit - KT_ToggleDungeonCredit) - green
    // if credit is already granted (mq_dungeon_credit), red otherwise.
    // Clicking any entry (green or red) flips its status - needed for cases
    // where the auto-tracker doesn't see a particular dungeon
    // (Maraudon/Razorfen Downs etc.), or to deliberately remove credit.
    static void ShowDungeonContinent(Player* player, Creature* creature, uint32 continentIdx)
    {
        player->PlayerTalkClass->ClearMenus();

        auto dungeons = KT_GetAllDungeons();
        auto credited = KT_GetCreditedDungeonEntries(player);
        std::set<uint32> creditedSet(credited.begin(), credited.end());

        std::vector<std::string> continentNames;
        std::vector<std::vector<KTDungeonEntry const*>> groups;
        for (auto const& d : dungeons)
        {
            if (continentNames.empty() || continentNames.back() != d.continent)
            {
                continentNames.push_back(d.continent);
                groups.push_back({});
            }
            groups.back().push_back(&d);
        }

        if (continentIdx >= uint32(groups.size()))
        {
            ShowDungeons(player, creature);
            return;
        }

        AddInfoLine(player, "|cffffd700" + continentNames[continentIdx] + "|r (click a dungeon to grant/remove credit manually):", ACT_DUNGEON_CONTINENT_BASE + continentIdx);

        // Safety cap (the real dungeon count per continent is single/
        // double-digit, so this practically never triggers) - the same
        // protection against exceeding GOSSIP_MAX_MENU_ITEMS=32 as KT_UI_LOCATION_BOSS_CAP.
        uint32 shown = 0;
        for (KTDungeonEntry const* d : groups[continentIdx])
        {
            if (shown >= KT_UI_LOCATION_BOSS_CAP)
            {
                AddInfoLine(player, Acore::StringFormat(
                    "{}... and {} more (list too long for one menu)|r",
                    KT_GRAY_UI, uint32(groups[continentIdx].size()) - shown), ACT_DUNGEON_CONTINENT_BASE + continentIdx);
                break;
            }

            bool done = creditedSet.count(d->creditEntry) != 0;
            std::string text = (done ? std::string("|cff2ecc40") : std::string("|cff8b0000")) + d->name
                + FormatDungeonLevelSuffix(*d) + "|r";
            uint32 globalIdx = uint32(d - dungeons.data());
            AddGossipItemFor(player, done ? GOSSIP_ICON_INTERACT_1 : GOSSIP_ICON_TALK, text,
                SENDER_NAV, ACT_DUNGEON_TOGGLE_BASE + globalIdx);
            ++shown;
        }

        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "|TInterface/ICONS/INV_Misc_Note_01:24:24:-18|t[<- Dungeons]", SENDER_NAV, ACT_DUNGEONS);
        AddBackButton(player);
        player->PlayerTalkClass->SendGossipMenu(1, creature->GetGUID());
    }

    // ---- Teleport to Dungeon ----
    // IMPLEMENTATION: NOT a direct TeleportTo (the first version did this -
    // and it turned out to be not always safe/correct, both for a point
    // inside the instance and even OUTSIDE, from the portal-side perspective
    // the player "enters" from).
    // Instead, clicking queues the player via LFGMgr::JoinLfg(), the SAME
    // path used by the standard "Looking For Group" (Dungeon Finder) window -
    // the server itself handles the teleport once the proposal forms (the
    // same "Group formed for: ..." popup with a [Enter Dungeon] button that
    // a regular player would see). This server has mod-solo-lfg installed
    // (SoloLFG.Enable=1 in its config) - so the proposal forms INSTANTLY
    // even for a single person with no group, no need to wait for real other players.
    //
    // Top level - list of continents, same approach/grouping as
    // ShowDungeons() above (KT_GetAllDungeons(), continent -> name), BUT
    // without any credit tie-in - entries here are purely navigational.
    static void ShowTeleportDungeons(Player* player, Creature* creature)
    {
        player->PlayerTalkClass->ClearMenus();

        auto dungeons = KT_GetAllDungeons();

        AddInfoLine(player, "|cffffd700Looking For Group|r - choose a continent, then a dungeon:", ACT_TELEPORT);

        if (dungeons.empty())
        {
            AddInfoLine(player, std::string(KT_GRAY_UI) + "No known dungeons found on the server.|r", ACT_TELEPORT);
        }
        else
        {
            std::vector<std::pair<std::string, uint32>> continents; // name, count
            for (auto const& d : dungeons)
            {
                if (continents.empty() || continents.back().first != d.continent)
                    continents.push_back({ d.continent, 0 });
                ++continents.back().second;
            }

            for (uint32 c = 0; c < uint32(continents.size()); ++c)
                AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                    continents[c].first + " (" + std::to_string(continents[c].second) + ")",
                    SENDER_NAV, ACT_TP_CONTINENT_BASE + c);
        }

        AddBackButton(player);
        player->PlayerTalkClass->SendGossipMenu(1, creature->GetGUID());
    }

    // Second level - flat list of the chosen continent's dungeons. All
    // entries are clickable (every KTDungeonEntry in KT_GetAllDungeons() is
    // guaranteed to have a valid lfgDungeonId - otherwise it wouldn't have
    // made it into this list at all, see KT_GetAllDungeons() in
    // kill_milestones.cpp) - unlike a direct TeleportTo, no separate "is
    // there a known entrance" check is needed here.
    static void ShowTeleportContinent(Player* player, Creature* creature, uint32 continentIdx)
    {
        player->PlayerTalkClass->ClearMenus();

        auto dungeons = KT_GetAllDungeons();
        // The same "live" query as ShowDungeonContinent (not cached between
        // menu openings) - so the green/red color here is AUTOMATICALLY in
        // sync with the [Cleared Dungeons] list: as soon as credit is
        // granted (boss killed/manual toggle), the next time this menu
        // opens it already shows the dungeon in green.
        auto credited = KT_GetCreditedDungeonEntries(player);
        std::set<uint32> creditedSet(credited.begin(), credited.end());

        std::vector<std::string> continentNames;
        std::vector<std::vector<KTDungeonEntry const*>> groups;
        for (auto const& d : dungeons)
        {
            if (continentNames.empty() || continentNames.back() != d.continent)
            {
                continentNames.push_back(d.continent);
                groups.push_back({});
            }
            groups.back().push_back(&d);
        }

        if (continentIdx >= uint32(groups.size()))
        {
            ShowTeleportDungeons(player, creature);
            return;
        }

        AddInfoLine(player, "|cffffd700" + continentNames[continentIdx] + "|r (click a name to queue for LFG):", ACT_TP_CONTINENT_BASE + continentIdx);

        uint32 shown = 0;
        for (KTDungeonEntry const* d : groups[continentIdx])
        {
            if (shown >= KT_UI_LOCATION_BOSS_CAP)
            {
                AddInfoLine(player, Acore::StringFormat(
                    "{}... and {} more (list too long for one menu)|r",
                    KT_GRAY_UI, uint32(groups[continentIdx].size()) - shown), ACT_TP_CONTINENT_BASE + continentIdx);
                break;
            }

            uint32 globalIdx = uint32(d - dungeons.data());
            bool done = creditedSet.count(d->creditEntry) != 0;

            // The same color code as [Cleared Dungeons]: green = credit
            // already granted, red = not yet (here it's purely informational,
            // clicking queues LFG either way).
            std::string text = (done ? std::string("|cff2ecc40") : std::string("|cff8b0000"))
                + d->name + FormatDungeonLevelSuffix(*d) + "|r";
            AddGossipItemFor(player, done ? GOSSIP_ICON_INTERACT_1 : GOSSIP_ICON_TALK, text,
                SENDER_NAV, ACT_TP_ITEM_BASE + globalIdx);
            ++shown;
        }

        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "|TInterface/ICONS/INV_Misc_Note_01:24:24:-18|t[<- Looking For Group]", SENDER_NAV, ACT_TELEPORT);
        AddBackButton(player);
        player->PlayerTalkClass->SendGossipMenu(1, creature->GetGUID());
    }

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        if (!KillTrackerEnable)
            return false;
        ShowMainMenu(player, creature);
        return true;
    }

    bool OnGossipSelect(Player* player, Creature* creature, uint32 /*sender*/, uint32 action) override
    {
        if (!KillTrackerEnable)
            return false;

        if (action >= ACT_ELITE_LIST_PAGE_BASE && action < ACT_ELITE_LIST_PAGE_BASE + 1000)
        {
            ShowEliteBonuses(player, creature, action - ACT_ELITE_LIST_PAGE_BASE);
            return true;
        }
        if (action >= ACT_SKILLS_LIST_PAGE_BASE && action < ACT_SKILLS_LIST_PAGE_BASE + 1000)
        {
            ShowSkillBonuses(player, creature, action - ACT_SKILLS_LIST_PAGE_BASE);
            return true;
        }
        if (action >= ACT_BOSS_CONTINENT_BASE && action < ACT_BOSS_CONTINENT_BASE + 100)
        {
            ShowBossContinent(player, creature, action - ACT_BOSS_CONTINENT_BASE, 0);
            return true;
        }
        if (action >= ACT_BOSS_LOC_PAGE_BASE && action < ACT_BOSS_LOC_PAGE_BASE + 1000)
        {
            uint32 off = action - ACT_BOSS_LOC_PAGE_BASE;
            ShowBossContinent(player, creature, off / 100, off % 100);
            return true;
        }
        if (action >= ACT_BOSS_LOCATION_BASE && action < ACT_BOSS_LOCATION_BASE + 1000)
        {
            ShowBossLocation(player, creature, action - ACT_BOSS_LOCATION_BASE);
            return true;
        }
        if (action >= ACT_BOSS_ITEM_BASE && action < ACT_BOSS_ITEM_BASE + 2000)
        {
            HandleBossItemClick(player, creature, action - ACT_BOSS_ITEM_BASE);
            return true;
        }
        if (action >= ACT_DUNGEON_CONTINENT_BASE && action < ACT_DUNGEON_CONTINENT_BASE + 100)
        {
            ShowDungeonContinent(player, creature, action - ACT_DUNGEON_CONTINENT_BASE);
            return true;
        }
        if (action >= ACT_DUNGEON_TOGGLE_BASE && action < ACT_DUNGEON_TOGGLE_BASE + 1000)
        {
            auto dungeons = KT_GetAllDungeons();
            uint32 globalIdx = action - ACT_DUNGEON_TOGGLE_BASE;
            if (globalIdx >= uint32(dungeons.size()))
            {
                ShowDungeons(player, creature);
                return true;
            }

            uint32 creditEntry = dungeons[globalIdx].creditEntry;
            KT_ToggleDungeonCredit(player, creditEntry);

            ShowDungeonContinent(player, creature, FindContinentIndexForDungeon(dungeons, creditEntry));
            return true;
        }
        if (action >= ACT_TP_CONTINENT_BASE && action < ACT_TP_CONTINENT_BASE + 100)
        {
            ShowTeleportContinent(player, creature, action - ACT_TP_CONTINENT_BASE);
            return true;
        }
        if (action >= ACT_TP_ITEM_BASE && action < ACT_TP_ITEM_BASE + 1000)
        {
            auto dungeons = KT_GetAllDungeons();
            uint32 globalIdx = action - ACT_TP_ITEM_BASE;
            if (globalIdx >= uint32(dungeons.size()))
            {
                ShowTeleportDungeons(player, creature);
                return true;
            }

            KTDungeonEntry const& d = dungeons[globalIdx];

            // Queue the player for LFG - the SAME call the standard "Looking
            // For Group" window makes via CMSG_LFG_JOIN
            // (WorldSession::HandleLfgJoinOpcode, LFGHandler.cpp): dungeons -
            // a set of RAW IDs from LFGDungeons.dbc (not Entry()! JoinLfg
            // itself handles type masking). All 3 roles are offered at once
            // (tank+heal+dps) - for the solo queue (mod-solo-lfg) the actual
            // role doesn't matter, this just maximizes the chance of an
            // instant match regardless of the player's class/spec. The
            // server itself sends the standard "Group formed for: ..." popup
            // with an [Enter Dungeon] button, and handles the teleport
            // correctly itself - the same path millions of regular players
            // have gone through, so there's none of the "wrong side of the
            // portal"/broken-bind risk that came with a direct TeleportTo.

            lfg::LfgDungeonSet lfgDungeons;
            lfgDungeons.insert(d.lfgDungeonId);
            sLFGMgr->JoinLfg(player,
                uint8(lfg::PLAYER_ROLE_TANK | lfg::PLAYER_ROLE_HEALER | lfg::PLAYER_ROLE_DAMAGE),
                lfgDungeons, "");
            player->UpdateLFGChannel();

            ChatHandler(player->GetSession()).PSendSysMessage("|cff00ff00Queued for LFG:|r {}", d.name);
            return true;
        }

        switch (action)
        {
            case ACT_STATUS:       ShowStatus(player, creature); return true;
            case ACT_TOP_MOBS:     ShowTopMobs(player, creature); return true;
            case ACT_LEADERBOARD:  ShowLeaderboard(player, creature); return true;
            case ACT_TITLES:       ShowTitles(player, creature); return true;
            case ACT_BOSS_BONUSES: ShowBossBonuses(player, creature); return true;
            case ACT_ELITE_BONUSES: ShowEliteBonuses(player, creature); return true;
            case ACT_CONTRACT:     ShowContract(player, creature); return true;
            case ACT_CONTRACT_REROLL:
            {
                if (!KT_RerollContract(player))
                    ChatHandler(player->GetSession()).PSendSysMessage(
                        "|cff8b0000Couldn't reload the contract|r - either it's already been completed today, or there are no suitable targets in the current zone.");
                ShowContract(player, creature);
                return true;
            }
            case ACT_WEEKLY_CONTRACT: ShowWeeklyContract(player, creature); return true;
            case ACT_DUNGEONS:      ShowDungeons(player, creature); return true;
            case ACT_SKILLS:        ShowSkillBonuses(player, creature); return true;
            case ACT_TELEPORT:      ShowTeleportDungeons(player, creature); return true;
            default:                ShowMainMenu(player, creature); return true;
        }
    }
};

class KillTrackerConfig : public WorldScript
{
public:
    KillTrackerConfig() : WorldScript("KillTrackerConfig", { WORLDHOOK_ON_BEFORE_CONFIG_LOAD }) { }

    void OnBeforeConfigLoad(bool reload) override
    {
        if (!reload)
            KillTrackerEnable = sConfigMgr->GetOption<bool>("KillTracker.Enable", true);
    }
};

void AddKillMilestonesScripts();

void AddKillTrackerScripts()
{
    new KillTrackerConfig();
    new npc_kill_tracker();
    AddKillMilestonesScripts();
}
