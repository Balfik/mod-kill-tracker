// Rewards for killing enemies - the same approach as quest_milestones.cpp
// in mod-missed-quests: a permanent passive spell (learnSpell) is granted
// once, when the counter first crosses a threshold, and is never removed
// (tiers stack). Here the counter is "kills" (the player's killing blow on
// any Creature, both normal mobs and elite/bosses), not quests.
//
// Besides the tier buffs, this file also implements:
//   - streak (kill streak without dying) - a temporary buff, reset on death
//   - personal top-5 most-killed creature types
//   - server-wide leaderboard of all characters by total_kills
//   - cosmetic "titles" (text only, not a real client Title - see the
//     comment near KT_GetTitleName in kill_milestones.h) every 10 tiers,
//     each new title grants gold (titleNumber * 100 gold)
//   - for EVERY new (never-before-killed) boss - a permanent +5% to all
//     stats (a separate independent spell per boss, stacking)

#include "Chat.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "Group.h"
#include "MapMgr.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "SpellAuraDefines.h"
#include "SpellAuras.h"
#include "StringFormat.h"
#include "World.h"

// mod-multi-trainer - only for the "Abilities Unlocked" line on the HUD's
// [Bonuses] tab (MT_GetLearnedPassiveCount/MT_GetTotalPassiveCount). Safe
// (verified): MODULES=static in build/CMakeCache.txt - all custom modules
// are compiled into one static library sharing one include path.
#include "mt_api.h"
#include "WorldPacket.h"
#include "WorldSessionMgr.h"

#include "kill_milestones.h"

#include <algorithm>
#include <ctime>
#include <map>
#include <set>

// In process memory - the source of truth for threshold checks (no delay
// from an async DB write). Persistence in mq_kill_stats - for surviving
// restarts/relogs, written asynchronously (Execute), since kills happen
// far more often than quest completions.
static std::map<uint32 /*guid*/, KTStats> g_stats;

// Daily contracts - a separate cache (not in g_stats, since it holds
// strings/coordinates not needed by the rest of the system). Lives as long
// as the player is logged in (same as g_stats).

// "Day" of the last counted kill for each player (year*1000+day_of_year) -
// so a midnight rollover is caught even WITHOUT a relog/server restart
// (daily_date in the DB itself only updates on a kill, so without this
// check the in-memory dailyKills counter would never reset for a player
// who hasn't logged out in over a day).
static std::map<uint32 /*guid*/, uint32> g_dailyDayKey;

// "Day" of the last generated contract (same approach as g_dailyDayKey
// above) - without this, g_contracts would cache a contract forever in
// memory, ignoring a midnight rollover if the player never logs out.
static std::map<uint32 /*guid*/, uint32> g_contractDayKey;

static uint32 CurrentDayKey()
{
    time_t now = time(nullptr);
    tm local {};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    return uint32(local.tm_year) * 1000 + uint32(local.tm_yday);
}

// Weekly contracts - a separate cache (same approach as g_contracts/
// g_contractDayKey for the daily one, just on a week boundary instead of a day).
static std::map<uint32 /*guid*/, KTWeeklyContract> g_weeklyContracts;
static std::map<uint32 /*guid*/, uint32> g_weeklyWeekKey;

// "Week" (year*100 + week number in the year, ISO-like via tm_yday/7 - THIS
// IS NOT a real ISO 8601 week (that would be counted from Thursday/Monday
// with year-boundary handling), just a simple "days_since_year_start / 7" -
// good enough for our purposes: all that matters is that the boundary is
// SHARED and monotonic for all players at once, not exact calendar-week
// alignment).
{
    time_t now = time(nullptr);
    tm local {};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    return uint32(local.tm_year) * 100 + uint32(local.tm_yday) / 7;
}

// Seconds remaining until the next weekly contract reset - the same
// "days_since_year_start/7" boundary as CurrentWeekKey, expressed in
// seconds until the midnight on which (tm_yday+1)/7 first becomes a
// different number from the current one.
static uint32 SecondsUntilNextWeekReset()
{
    time_t now = time(nullptr);
    tm local {};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    uint32 curWeek = uint32(local.tm_yday) / 7;
    uint32 daysIntoWeek = uint32(local.tm_yday) % 7;
    uint32 daysRemaining = 6 - daysIntoWeek; // full days left in the current "week"
    int secondsPassedToday = local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;
    int remain = int(daysRemaining) * 24 * 3600 + (24 * 3600 - secondsPassedToday);
    if (remain < 0) remain = 0;
    (void)curWeek;
    return uint32(remain);
}

// Seconds remaining until the next midnight by server system time - the
// same moment CURDATE() (and therefore the daily contract) rolls over to a
// new day. The same countdown for all players (the reset boundary itself
// is shared), even though the CONTENT of the new contract is individual
// per player (generated separately per guid on first access each new day).
static uint32 SecondsUntilNextMidnight()
{
    time_t now = time(nullptr);
    tm local {};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    int secondsPassedToday = local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;
    int remain = 24 * 3600 - secondsPassedToday;
    if (remain < 0)
        remain = 0;
    return uint32(remain);
}

static uint32 GetGuid(Player* player)
{
    return player->GetGUID().GetCounter();
}


KTStats& GetOrLoadStats(Player* player)
{
    uint32 guid = GetGuid(player);
    auto it = g_stats.find(guid);
    if (it != g_stats.end())
        return it->second;

    KTStats stats;
    if (QueryResult result = CharacterDatabase.Query(
            "SELECT total_kills, normal_kills, elite_kills, boss_kills, streak_kills, best_streak, "
            "DATEDIFF(NOW(), first_kill_at), unique_bosses_killed, daily_kills, daily_completions, "
            "(daily_date = CURDATE()), dungeons_cleared FROM mq_kill_stats WHERE guid = {}", guid))
    {
        Field* f = result->Fetch();
        stats.total             = f[0].Get<uint32>();
        stats.normal            = f[1].Get<uint32>();
        stats.elite              = f[2].Get<uint32>();
        stats.boss               = f[3].Get<uint32>();
        stats.streak             = f[4].Get<uint32>();
        stats.bestStreak         = f[5].Get<uint32>();
        stats.daysTracked        = f[6].Get<uint32>();
        stats.uniqueBossesKilled = f[7].Get<uint32>();
        stats.dailyCompletions   = f[9].Get<uint32>();
        stats.uniqueDungeonsCleared = f[11].Get<uint32>();

        // daily_kills in the DB is only valid if the stored daily_date == today;
        // otherwise the day has already changed (the server didn't restart at
        // midnight) - treat the counter as reset on the client, the DB will
        // update on the first kill today.
        stats.dailyKills = sameDay ? f[8].Get<uint32>() : 0;
    }
    else
    {
        CharacterDatabase.Execute("INSERT INTO mq_kill_stats (guid) VALUES ({})", guid);
    }

    // Mark "today" for the in-memory midnight check (OnPlayerCreatureKill) -
    // without this, the very first kill after login would incorrectly reset
    // the daily_kills value just correctly loaded from the DB.
    g_dailyDayKey[guid] = CurrentDayKey();

    return g_stats[guid] = stats;
}

// ---- Tier (permanent) buffs ----

static void GrantKillTierIfReached(Player* player, uint32 totalKills, uint32 tier)
{
    uint32 threshold = tier * KT_MILESTONE_STEP;
    if (totalKills < threshold)
        return;

    uint32 spellA = KT_MILESTONE_BASE_SPELL_A + (tier - 1);
    uint32 spellB = KT_MILESTONE_BASE_SPELL_B + (tier - 1);

    bool hadA = player->HasSpell(spellA);
    bool hadB = player->HasSpell(spellB);
    if (!hadA)
        player->learnSpell(spellA);
    if (!hadB)
        player->learnSpell(spellB);

    bool firstTime = !hadA || !hadB;
    if (!firstTime)
        return;

    ChatHandler(player->GetSession()).PSendSysMessage(
        "|cff00ccffMilestone:|r {}+ enemies killed - permanent bonus is now |cffffd700+{} armor, +{} attack power "
        "(melee/ranged/spell), +{}% attack speed (melee/ranged/cast)|r!",
        threshold, tier * KT_ARMOR_PER_TIER, tier * KT_ATTACK_POWER_PER_TIER, tier * KT_HASTE_PCT_PER_TIER);

    // Cosmetic title - every KT_TITLE_TIER_STEP tiers. titleNumber
    // (1-based, 1..10) determines both the name and the gold reward amount.
    if (tier % KT_TITLE_TIER_STEP == 0)
    {
        uint32 titleNumber = tier / KT_TITLE_TIER_STEP;
        uint32 gold = titleNumber * KT_TITLE_GOLD_STEP;
        player->ModifyMoney(int32(gold));

        ChatHandler(player->GetSession()).PSendSysMessage(
            "|cffff8000New title unlocked:|r \"{}\" - reward {} gold! (see the full list in the [Titles] menu "
            "at the Hunt Chronicler)",
            KT_GetTitleName(titleNumber - 1), gold / 10000);
    }
}

static void CheckKillMilestones(Player* player, KTStats const& stats)
{
    for (uint32 tier = 1; tier <= KT_MILESTONE_MAX_TIER; ++tier)
        GrantKillTierIfReached(player, stats.total, tier);
}

// ---- Elite/rare bonus (every KT_ELITE_TIER_STEP such kills) ----
// Counted from stats.elite - the same elite_kills column shown in Status,
// now shared with the group (GrantEliteCreditToPlayer/ShareGroupKillCredit below).

static void GrantEliteTierIfReached(Player* player, uint32 eliteCredit, uint32 tier)
{
    uint32 threshold = tier * KT_ELITE_TIER_STEP;
    if (eliteCredit < threshold)
        return;

    uint32 spellId = KT_ELITE_BONUS_BASE_SPELL + (tier - 1);
    if (player->HasSpell(spellId))
        return;

    player->learnSpell(spellId);

    ChatHandler(player->GetSession()).PSendSysMessage(
        "|cffff8000Elite Hunter:|r {}+ elite/rare enemies credited - permanent bonus is now "
        "|cffffd700+{}% to all stats|r!",
        threshold, tier * KT_ELITE_STAT_PCT_PER_TIER);
}

// A second, PARALLEL bonus on the same tiers - accuracy (melee+ranged
// combined via one SPELL_AURA_MOD_HIT_CHANCE aura, and spell hit separately
// via SPELL_AURA_MOD_SPELL_HIT_CHANCE) - see the comment near KT_ELITE_HIT_*
// in kill_milestones.h.
static void GrantEliteAccuracyBonusIfReached(Player* player, uint32 eliteCredit, uint32 tier)
{
    uint32 threshold = tier * KT_ELITE_TIER_STEP;
    if (eliteCredit < threshold)
        return;

    uint32 spellId = KT_ELITE_HIT_BONUS_BASE_SPELL + (tier - 1);
    if (player->HasSpell(spellId))
        return;

    player->learnSpell(spellId);

    ChatHandler(player->GetSession()).PSendSysMessage(
        "|cffff8000Elite Hunter:|r {}+ elite/rare enemies credited - permanent bonus is now "
        "|cffffd700+{}% hit chance (melee/ranged) and +{}% spell hit chance|r!",
        threshold, tier * KT_ELITE_HIT_PCT_PER_TIER, tier * KT_ELITE_HIT_PCT_PER_TIER);
}

static void CheckEliteTierMilestones(Player* player, uint32 eliteCredit)
{
    for (uint32 tier = 1; tier <= KT_ELITE_MAX_TIER; ++tier)
    {
        GrantEliteTierIfReached(player, eliteCredit, tier);
        GrantEliteAccuracyBonusIfReached(player, eliteCredit, tier);
    }
}

// Credits an elite/rare kill to PERSONAL stats (stats.elite, the same
// elite_kills column shown in Status) - called for both the killer and
// party members in ShareGroupKillCredit below, so a kill by a teammate
// counts exactly as if the player had killed it themselves.
static void GrantEliteCreditToPlayer(Player* p)
{
    uint32 guid = GetGuid(p);
    KTStats& pstats = GetOrLoadStats(p);

    ++pstats.elite;
    CharacterDatabase.Execute(
        "UPDATE mq_kill_stats SET elite_kills = elite_kills + 1 WHERE guid = {}", guid);
    CheckEliteTierMilestones(p, pstats.elite);
}

// ---- Streak (no-death kill chain) ----

static void GrantStreakBuffIfReached(Player* player, uint32 streak)
{
    if (streak == 0 || streak % KT_STREAK_THRESHOLD != 0)
        return;

    player->CastSpell(player, KT_STREAK_BUFF_SPELL, true);
    if (Aura* aura = player->GetAura(KT_STREAK_BUFF_SPELL, player->GetGUID()))
    {
        aura->SetMaxDuration(KT_STREAK_BUFF_DURATION_MS);
        aura->SetDuration(KT_STREAK_BUFF_DURATION_MS);
    }

    ChatHandler(player->GetSession()).PSendSysMessage(
        "|cff00ccffKill streak:|r {} in a row without dying! Gained \"Battle Fervor\" (+{}% damage from all sources, "
        "15 min). Don't die or you'll lose the streak!",
        streak, KT_STREAK_BUFF_DAMAGE_PCT);
}

// ---- New boss bonus (+5% to all stats per EVERY new boss) ----

// tier here = the sequential index of the unique boss (1..KT_BOSS_MAX_UNIQUE),
// unrelated to the KT_MILESTONE_STEP kill tier - a separate spell range.
static void GrantBossBonusIfReached(Player* player, uint32 uniqueBossesKilled, uint32 tier, std::string const& justKilledName)
{
    if (uniqueBossesKilled < tier)
        return;

    uint32 spellId = KT_BOSS_BONUS_BASE_SPELL + (tier - 1);
    if (player->HasSpell(spellId))
        return;

    player->learnSpell(spellId);

    ChatHandler(player->GetSession()).PSendSysMessage(
        "|cffff4040New boss defeated:|r \"{}\" - permanent bonus is now |cffffd700+{}% to all stats|r "
        "(total unique bosses: {})!",
        justKilledName, tier * KT_BOSS_STAT_PCT_PER_BOSS, uniqueBossesKilled);
}

static void CheckBossBonusMilestones(Player* player, uint32 uniqueBossesKilled, std::string const& justKilledName)
{
    for (uint32 tier = 1; tier <= KT_BOSS_MAX_UNIQUE; ++tier)
        GrantBossBonusIfReached(player, uniqueBossesKilled, tier, justKilledName);
}

// Independent of mq_kill_stats_by_creature check for "this player received
// credit for this boss for the first time" - via a separate mq_boss_credit
// table (guid, creature_entry). Deliberately DECOUPLED from the personal
// kill counter for this creature type - so it works the same for whoever
// landed the killing blow and for party members (see GrantSharedKillCredit),
// whose personal kill counter is never incremented at all.
static void CheckNewBossKill(Player* player, KTStats& stats, uint32 creatureEntry, bool isBoss)
{
    if (!isBoss)
        return;

    uint32 guid = GetGuid(player);

    if (QueryResult already = CharacterDatabase.Query(
            "SELECT 1 FROM mq_boss_credit WHERE guid = {} AND creature_entry = {}", guid, creatureEntry))
        return; // this player has already been credited for this boss

    CharacterDatabase.Execute(
        "INSERT INTO mq_boss_credit (guid, creature_entry) VALUES ({}, {})", guid, creatureEntry);

    ++stats.uniqueBossesKilled;
    CharacterDatabase.Execute(
        "UPDATE mq_kill_stats SET unique_bosses_killed = {} WHERE guid = {}", stats.uniqueBossesKilled, guid);

    std::string name = "the boss";
    if (CreatureTemplate const* ct = sObjectMgr->GetCreatureTemplate(creatureEntry))
        name = ct->Name;

    CheckBossBonusMilestones(player, stats.uniqueBossesKilled, name);
}

// ---- Daily quota ----

static void GrantDailyXpBuffIfReached(Player* player, KTStats& stats, uint32 guid)
{
    if (stats.dailyKills < KT_DAILY_NORM)
        return;

    // The quota was JUST reached (crossed the threshold with this very kill) -
    // otherwise dailyCompletions already accounts for today's completion and
    // won't fire again until the next daily reset.
    if (stats.dailyKills != KT_DAILY_NORM)
        return;

    if (stats.dailyCompletions >= KT_DAILY_MAX_COMPLETIONS)
        return;

    ++stats.dailyCompletions;
    uint32 spellId = KT_DAILY_XP_BUFF_BASE_SPELL + (stats.dailyCompletions - 1);
    player->learnSpell(spellId);

    CharacterDatabase.Execute(
        "UPDATE mq_kill_stats SET daily_completions = {} WHERE guid = {}", stats.dailyCompletions, guid);

    ChatHandler(player->GetSession()).PSendSysMessage(
        "|cff00ff00Daily quota complete!|r {}+ enemies killed today - permanent bonus is now "
        "|cffffd700+{}% XP|r (total completions: {}).",
        KT_DAILY_NORM, stats.dailyCompletions, stats.dailyCompletions);
}

// ---- Cosmetic trophies (every KT_TROPHY_TIER_STEP tiers) ----

static void GrantTrophyIfReached(Player* player, uint32 totalKills)
{
    uint32 tier = totalKills / KT_MILESTONE_STEP;
    if (tier == 0 || tier % KT_TROPHY_TIER_STEP != 0)
        return;

    uint32 trophyIndex = tier / KT_TROPHY_TIER_STEP; // 1..5
    uint32 itemId = KT_TROPHY_BASE_ITEM + (trophyIndex - 1);

    if (player->HasItemCount(itemId, 1, true))
        return; // already received (also checked against bank/mail - the trophy is unique)

    if (player->AddItem(itemId, 1))
        ChatHandler(player->GetSession()).PSendSysMessage(
            "|cffff8000Hunter's Trophy!|r For {} enemies killed (tier {}) a cosmetic trophy weapon "
            "was added to your bag - appearance only, no stats.", tier * KT_MILESTONE_STEP, tier);
}

// ---- Favorite farming zone ----

static void TrackZoneKill(Player* player, uint32 guid)
{
    uint32 zoneId = player->GetZoneId();
    if (zoneId == 0)
        return;

    std::string zoneName = "Unknown Zone";
    if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(zoneId))
        zoneName = area->area_name[LOCALE_enUS];

    // Escape the zone name - some contain an apostrophe (e.g. "Un'Goro Crater"),
    // without this the INSERT would break SQL syntax.
    CharacterDatabase.EscapeString(zoneName);
    CharacterDatabase.Execute(
        "INSERT INTO mq_kill_stats_by_zone (guid, zone_id, zone_name, kills) VALUES ({}, {}, '{}', 1) "
        "ON DUPLICATE KEY UPDATE kills = kills + 1", guid, zoneId, zoneName);
}

std::vector<KTZoneKillEntry> KT_GetTopZones(Player* player, uint32 limit)
{
    std::vector<KTZoneKillEntry> out;
    uint32 guid = GetGuid(player);

    QueryResult result = CharacterDatabase.Query(
        "SELECT zone_id, zone_name, kills FROM mq_kill_stats_by_zone WHERE guid = {} "
        "ORDER BY kills DESC LIMIT {}", guid, limit);
    if (!result)
        return out;

    do
    {
        Field* f = result->Fetch();
        KTZoneKillEntry entry;
        entry.zoneId = f[0].Get<uint32>();
        entry.zoneName = f[1].Get<std::string>();
        entry.kills = f[2].Get<uint32>();
        out.push_back(std::move(entry));
    } while (result->NextRow());

    return out;
}

std::vector<KTContractHistoryEntry> KT_GetContractHistory(Player* player, uint32 limit)
{
    std::vector<KTContractHistoryEntry> out;
    uint32 guid = GetGuid(player);

    QueryResult result = CharacterDatabase.Query(
        "SELECT contract_date, creature_name, area_name, reward_xp FROM mq_contract_history "
        "WHERE guid = {} ORDER BY contract_date DESC, id DESC LIMIT {}", guid, limit);
    if (!result)
        return out;

    do
    {
        Field* f = result->Fetch();
        KTContractHistoryEntry entry;
        entry.date         = f[0].Get<std::string>();
        entry.creatureName = f[1].Get<std::string>();
        entry.areaName     = f[2].Get<std::string>();
        entry.rewardXp     = f[3].Get<uint32>();
        out.push_back(std::move(entry));
    } while (result->NextRow());

    return out;
}

// ---- Daily contract ----

// Picks a random spawn point of some killable (not civilian/critter) mob on
// the player's current map, whose zone (via Map::GetZoneId from the spawn
// coordinates) matches the player's current zone. There are few candidates
// (dozens per zone) - a DB query + C++ zone check is acceptable performance-wise
// (runs once per day).
{
    uint32 mapId = player->GetMapId();
    uint32 zoneId = player->GetZoneId();

    QueryResult candidates = WorldDatabase.Query(
        "SELECT c.id, c.position_x, c.position_y, c.position_z, ct.Name FROM creature c "
        "JOIN creature_template ct ON ct.entry = c.id "
        "WHERE c.map = {} AND ct.rank IN ({}, {}, {}) AND ct.type <> {} "
        "ORDER BY RAND() LIMIT 50",
        mapId, uint32(CREATURE_ELITE_NORMAL), uint32(CREATURE_ELITE_RARE), uint32(CREATURE_ELITE_ELITE),
        uint32(CREATURE_TYPE_CRITTER));
    if (!candidates)
        return false;

    do
    {
        Field* f = candidates->Fetch();
        float x = f[1].Get<float>();
        float y = f[2].Get<float>();
        float z = f[3].Get<float>();

        uint32 candidateZone = sMapMgr->GetZoneId(PHASEMASK_NORMAL, mapId, x, y, z);
        if (candidateZone != zoneId)
            continue;

        outEntry = f[0].Get<uint32>();
        outName  = f[4].Get<std::string>();
        outX = x; outY = y; outZ = z;
        return true;
    } while (candidates->NextRow());

    return false;
}

static uint32 CalcContractRewardXp(uint8 level)
{
    if (level <= KT_CONTRACT_XP_BASE_LEVEL)
        return KT_CONTRACT_XP_BASE;
    return KT_CONTRACT_XP_BASE + (uint32(level) - KT_CONTRACT_XP_BASE_LEVEL) * KT_CONTRACT_XP_PER_LEVEL;
}

KTDailyContract const& KT_GetOrGenerateContract(Player* player)
{
    uint32 guid = GetGuid(player);

    // BUG found during review: the g_contracts cache was previously never
    // compared against the current date - if a player didn't log out through
    // midnight, this function would forever return YESTERDAY's (stale)
    // contract from memory, ignoring that a new day had started. Fixed with
    // the same approach as daily_kills (g_dailyDayKey) - the "day" is checked
    // on every access and the cache reset if it changed.
    uint32 today = CurrentDayKey();
    auto contractDayIt = g_contractDayKey.find(guid);
    if (contractDayIt != g_contractDayKey.end() && contractDayIt->second != today)
        g_contracts.erase(guid);
    g_contractDayKey[guid] = today;

    auto it = g_contracts.find(guid);
    if (it != g_contracts.end())
        return it->second;

    KTDailyContract contract;

    if (QueryResult result = CharacterDatabase.Query(
            "SELECT creature_entry, creature_name, area_name, pos_x, pos_y, pos_z, zone_id, "
            "reward_xp, completed FROM mq_daily_contract WHERE guid = {} AND contract_date = CURDATE()", guid))
    {
        Field* f = result->Fetch();
        contract.hasContract    = true;
        contract.creatureEntry  = f[0].Get<uint32>();
        contract.creatureName   = f[1].Get<std::string>();
        contract.areaName       = f[2].Get<std::string>();
        contract.posX           = f[3].Get<float>();
        contract.posY           = f[4].Get<float>();
        contract.posZ           = f[5].Get<float>();
        contract.zoneId         = f[6].Get<uint32>();
        contract.rewardXp       = f[7].Get<uint32>();
        contract.completed      = f[8].Get<uint32>() != 0;
    }
    else
    {
        uint32 entry; std::string name; float x, y, z;
        if (PickContractTarget(player, entry, name, x, y, z))
        {
            uint32 zoneId = player->GetZoneId();

            contract.hasContract   = true;
            contract.completed     = false;
            contract.creatureEntry = entry;
            contract.creatureName  = name;
            contract.areaName      = "Unknown Zone";
            if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(zoneId))
                contract.areaName = area->area_name[LOCALE_enUS];
            contract.posX = x; contract.posY = y; contract.posZ = z;
            contract.zoneId = zoneId;
            contract.rewardXp = CalcContractRewardXp(player->GetLevel());

            // Escape names - both the creature and the zone can contain an
            // apostrophe (e.g. "Ahn'Qiraj", "Grom'gol") - without this REPLACE would break SQL.
            std::string escName = name, escArea = contract.areaName;
            CharacterDatabase.EscapeString(escName);
            CharacterDatabase.EscapeString(escArea);
            CharacterDatabase.Execute(
                "REPLACE INTO mq_daily_contract (guid, contract_date, creature_entry, creature_name, area_name, "
                "pos_x, pos_y, pos_z, zone_id, reward_xp, completed) "
                "VALUES ({}, CURDATE(), {}, '{}', '{}', {}, {}, {}, {}, {}, 0)",
                guid, entry, escName, escArea, x, y, z, zoneId, contract.rewardXp);
        }
        // if no target was found (empty zone) - hasContract stays false, the
        // NPC will tell the player to try again later/in another zone.
    }

    return g_contracts[guid] = contract;
}

// Forward declaration - defined much further below (KT_SendHudUpdate needs
// KT_GetOrGenerateContract/KT_GetOrGenerateWeeklyContract to already exist),
// but KT_RerollContract needs to call it right after a successful
// regeneration - otherwise the HUD addon wouldn't see the new target until relog.
static void KT_SendHudUpdate(Player* player);

// Forcibly regenerate the daily contract - see the comment in kill_milestones.h.
bool KT_RerollContract(Player* player)
{
    uint32 guid = GetGuid(player);

    // Make sure the cache/completed state is up to date (today) before
    // checking - the same protection against a "yesterday's" contract as at
    // the start of KT_GetOrGenerateContract.
    KTDailyContract const& current = KT_GetOrGenerateContract(player);
    if (current.completed)
        return false; // can't reroll a contract already completed today

    uint32 entry; std::string name; float x, y, z;
    if (!PickContractTarget(player, entry, name, x, y, z))
        return false; // no suitable targets in the player's current zone

    KTDailyContract contract;
    uint32 zoneId = player->GetZoneId();

    contract.hasContract   = true;
    contract.completed     = false;
    contract.creatureEntry = entry;
    contract.creatureName  = name;
    contract.areaName      = "Unknown Zone";
    if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(zoneId))
        contract.areaName = area->area_name[LOCALE_enUS];
    contract.posX = x; contract.posY = y; contract.posZ = z;
    contract.zoneId = zoneId;
    contract.rewardXp = CalcContractRewardXp(player->GetLevel());

    std::string escName = name, escArea = contract.areaName;
    CharacterDatabase.EscapeString(escName);
    CharacterDatabase.EscapeString(escArea);
    CharacterDatabase.Execute(
        "REPLACE INTO mq_daily_contract (guid, contract_date, creature_entry, creature_name, area_name, "
        "pos_x, pos_y, pos_z, zone_id, reward_xp, completed) "
        "VALUES ({}, CURDATE(), {}, '{}', '{}', {}, {}, {}, {}, {}, 0)",
        guid, entry, escName, escArea, x, y, z, zoneId, contract.rewardXp);

    g_contracts[guid] = contract;
    KT_SendHudUpdate(player); // live HUD addon update - without this the new target would only show up after a relog
    return true;
}

// Computes the contract's posX/posY in client map format (0-100%) LIVE from
// raw world coordinates - the same call used by .gps (cs_misc.cpp): raw
// world X/Y are passed as-is, the axes get SWAPPED INSIDE
// Map2ZoneCoordinates itself (the client map uses a different axis order).
void KT_GetContractMapCoords(KTDailyContract const& contract, float& outX, float& outY)
{
    outX = contract.posX;
    outY = contract.posY;
    Map2ZoneCoordinates(outX, outY, contract.zoneId);
}

static void CheckContractCompletion(Player* player, KTStats& /*stats*/, uint32 killedEntry, uint32 guid)
{
    KTDailyContract const& contract = KT_GetOrGenerateContract(player);
    if (!contract.hasContract || contract.completed || contract.creatureEntry != killedEntry)
        return;

    g_contracts[guid].completed = true;
    CharacterDatabase.Execute(
        "UPDATE mq_daily_contract SET completed = 1 WHERE guid = {} AND contract_date = CURDATE()", guid);

    // mq_daily_contract - the working row for "today" (PRIMARY KEY guid,
    // overwritten via REPLACE INTO daily) - keeps NO history. So every
    // completion is ADDITIONALLY written to a separate append-only
    // mq_contract_history table - for the [Daily Contract] -> "history" menu.
    std::string escName = contract.creatureName, escArea = contract.areaName;
    CharacterDatabase.EscapeString(escName);
    CharacterDatabase.EscapeString(escArea);
    CharacterDatabase.Execute(
        "INSERT INTO mq_contract_history (guid, contract_date, creature_name, area_name, reward_xp) "
        "VALUES ({}, CURDATE(), '{}', '{}', {})", guid, escName, escArea, contract.rewardXp);

    player->GiveXP(contract.rewardXp, nullptr);
    player->ModifyMoney(int32(KT_CONTRACT_GOLD_REWARD * 10000)); // gold in copper

    // Visible chat message immediately on completion - the user asked to
    // bring it back (a previous version removed it by mistake).
    ChatHandler(player->GetSession()).PSendSysMessage(
        "|cffffd700Daily contract complete!|r Target \"{}\" destroyed - {} XP + {} gold received.",
        contract.creatureName, contract.rewardXp, KT_CONTRACT_GOLD_REWARD);
}

// ---- Weekly bonus contract ----
//
// The target is chosen from the same WORLDBOSS location list used by the
// [Boss Bonuses] menu (KT_GetAllBossLocations) - no separate DB query
// needed. Selection rule (agreed with the user): a FULLY random boss from
// the list; if its minLevel is beyond the player's reach, do NOT look for
// the "closest by level" - instead immediately take the boss with the
// LOWEST minLevel in the whole list (the easiest possible target).
static bool PickWeeklyContractTarget(Player* player, KTBossEntry& outBoss)
{
    std::vector<KTBossEntry> bosses = KT_GetAllBossLocations();
    if (bosses.empty())
        return false;

    uint32 idx = urand(0, uint32(bosses.size()) - 1);
    KTBossEntry const& picked = bosses[idx];

    if (uint32(player->GetLevel()) >= picked.minLevel)
    {
        outBoss = picked;
        return true;
    }

    // The player's level doesn't reach the randomly chosen boss - take the
    // easiest one (lowest minLevel) from the list instead.
    KTBossEntry const* easiest = &bosses[0];
    for (KTBossEntry const& b : bosses)
        if (b.minLevel < easiest->minLevel)
            easiest = &b;

    outBoss = *easiest;
    return true;
}

static uint32 CalcWeeklyContractRewardXp(uint8 level)
{
    // By agreement - twice what a daily contract would give at the same
    // level.
    return CalcContractRewardXp(level) * 2;
}

KTWeeklyContract const& KT_GetOrGenerateWeeklyContract(Player* player)
{
    uint32 guid = GetGuid(player);

    uint32 curWeek = CurrentWeekKey();
    auto weekIt = g_weeklyWeekKey.find(guid);
    if (weekIt != g_weeklyWeekKey.end() && weekIt->second != curWeek)
        g_weeklyContracts.erase(guid);
    g_weeklyWeekKey[guid] = curWeek;

    auto it = g_weeklyContracts.find(guid);
    if (it != g_weeklyContracts.end())
        return it->second;

    KTWeeklyContract contract;

    if (QueryResult result = CharacterDatabase.Query(
            "SELECT creature_entry, creature_name, continent, location, is_dungeon, "
            "has_own_pin, own_map_id, own_zone_id, own_pin_x, own_pin_y, "
            "has_entrance_pin, entrance_map_id, entrance_zone_id, entrance_pin_x, entrance_pin_y, "
            "reward_xp, completed FROM mq_weekly_contract WHERE guid = {} AND week_key = {}", guid, curWeek))
    {
        Field* f = result->Fetch();
        contract.hasContract    = true;
        contract.creatureEntry  = f[0].Get<uint32>();
        contract.creatureName   = f[1].Get<std::string>();
        contract.continent      = f[2].Get<std::string>();
        contract.location       = f[3].Get<std::string>();
        contract.isDungeon      = f[4].Get<uint32>() != 0;
        contract.hasOwnPin      = f[5].Get<uint32>() != 0;
        contract.ownMapId       = f[6].Get<uint32>();
        contract.ownZoneId      = f[7].Get<uint32>();
        contract.ownPinX        = f[8].Get<float>();
        contract.ownPinY        = f[9].Get<float>();
        contract.hasEntrancePin = f[10].Get<uint32>() != 0;
        contract.entranceMapId  = f[11].Get<uint32>();
        contract.entranceZoneId = f[12].Get<uint32>();
        contract.entrancePinX   = f[13].Get<float>();
        contract.entrancePinY   = f[14].Get<float>();
        contract.rewardXp       = f[15].Get<uint32>();
        contract.completed      = f[16].Get<uint32>() != 0;
    }
    else
    {
        KTBossEntry boss;
        if (PickWeeklyContractTarget(player, boss))
        {
            contract.hasContract   = true;
            contract.completed     = false;
            contract.creatureEntry = boss.creatureEntry;
            contract.creatureName  = boss.name;
            contract.continent     = boss.continent;
            contract.location      = boss.location;
            contract.isDungeon     = boss.isDungeon;
            contract.hasOwnPin      = boss.hasOwnPin;
            contract.ownMapId       = boss.ownMapId;
            contract.ownZoneId      = boss.ownZoneId;
            contract.ownPinX        = boss.ownPinX;
            contract.ownPinY        = boss.ownPinY;
            contract.hasEntrancePin = boss.hasEntrancePin;
            contract.entranceMapId  = boss.entranceMapId;
            contract.entranceZoneId = boss.entranceZoneId;
            contract.entrancePinX   = boss.entrancePinX;
            contract.entrancePinY   = boss.entrancePinY;
            contract.rewardXp = CalcWeeklyContractRewardXp(player->GetLevel());

            std::string escName = boss.name, escCont = boss.continent, escLoc = boss.location;
            CharacterDatabase.EscapeString(escName);
            CharacterDatabase.EscapeString(escCont);
            CharacterDatabase.EscapeString(escLoc);
            CharacterDatabase.Execute(
                "REPLACE INTO mq_weekly_contract (guid, week_key, creature_entry, creature_name, continent, "
                "location, is_dungeon, has_own_pin, own_map_id, own_zone_id, own_pin_x, own_pin_y, "
                "has_entrance_pin, entrance_map_id, entrance_zone_id, entrance_pin_x, entrance_pin_y, "
                "reward_xp, completed) VALUES ({}, {}, {}, '{}', '{}', '{}', {}, {}, {}, {}, {}, {}, "
                "{}, {}, {}, {}, {}, {}, 0)",
                guid, curWeek, boss.creatureEntry, escName, escCont, escLoc, boss.isDungeon ? 1 : 0,
                boss.hasOwnPin ? 1 : 0, boss.ownMapId, boss.ownZoneId, boss.ownPinX, boss.ownPinY,
                boss.hasEntrancePin ? 1 : 0, boss.entranceMapId, boss.entranceZoneId,
                boss.entrancePinX, boss.entrancePinY, contract.rewardXp);
        }
        // if the boss list is empty (shouldn't happen) - hasContract stays false.
    }

    return g_weeklyContracts[guid] = contract;
}

std::vector<KTContractHistoryEntry> KT_GetWeeklyContractHistory(Player* player, uint32 limit)
{
    std::vector<KTContractHistoryEntry> out;
    uint32 guid = GetGuid(player);

    QueryResult result = CharacterDatabase.Query(
        "SELECT completed_date, creature_name, location, reward_xp FROM mq_weekly_contract_history "
        "WHERE guid = {} ORDER BY completed_date DESC, id DESC LIMIT {}", guid, limit);
    if (!result)
        return out;

    do
    {
        Field* f = result->Fetch();
        KTContractHistoryEntry entry;
        entry.date         = f[0].Get<std::string>();
        entry.creatureName = f[1].Get<std::string>();
        entry.areaName      = f[2].Get<std::string>();
        entry.rewardXp      = f[3].Get<uint32>();
        out.push_back(std::move(entry));
    } while (result->NextRow());

    return out;
}

static void CheckWeeklyContractCompletion(Player* player, uint32 killedEntry, uint32 guid)
{
    KTWeeklyContract const& contract = KT_GetOrGenerateWeeklyContract(player);
    if (!contract.hasContract || contract.completed || contract.creatureEntry != killedEntry)
        return;

    g_weeklyContracts[guid].completed = true;
    CharacterDatabase.Execute(
        "UPDATE mq_weekly_contract SET completed = 1 WHERE guid = {} AND week_key = {}", guid, CurrentWeekKey());

    std::string escName = contract.creatureName, escLoc = contract.location;
    CharacterDatabase.EscapeString(escName);
    CharacterDatabase.EscapeString(escLoc);
    CharacterDatabase.Execute(
        "INSERT INTO mq_weekly_contract_history (guid, completed_date, creature_name, location, reward_xp) "
        "VALUES ({}, CURDATE(), '{}', '{}', {})", guid, escName, escLoc, contract.rewardXp);

    player->GiveXP(contract.rewardXp, nullptr);
    player->ModifyMoney(int32(KT_WEEKLY_GOLD_REWARD * 10000)); // gold in copper

    ChatHandler(player->GetSession()).PSendSysMessage(
        "|cffa335eeWeekly contract complete!|r Target \"{}\" destroyed - {} XP + {} gold received.",
        contract.creatureName, contract.rewardXp, KT_WEEKLY_GOLD_REWARD);
}

// A separate, VISIBLE message shown only on login (the same approach as
// KT_NotifyContractIfAvailable for the daily contract).
static void KT_NotifyWeeklyContractIfAvailable(Player* player)
{
    KTWeeklyContract const& contract = KT_GetOrGenerateWeeklyContract(player);
    if (contract.hasContract && !contract.completed)
    {
        ChatHandler(player->GetSession()).PSendSysMessage(
            "|cffa335eeWeekly contract:|r kill |cffff8000{}|r ({}) - reward {} XP + {} gold. "
            "See the \"Weekly\" tab in KillTrackerHUD or the NPC menu.",
            contract.creatureName, contract.location, contract.rewardXp, KT_WEEKLY_GOLD_REWARD);
    }
}

// ---- Group credit (daily quota / daily contract / boss bonus) ----
//
// Per the user's request, these 3 systems should be credited to the whole
// party, not just whoever landed the killing blow (personal total_kills/
// normal/elite/boss/streak/top-kills-by-type/favorite zone still count
// ONLY the player's own killing blow, as before; trophies also stay
// personal, since they're tied to the player's own tier).
// Declared below (near KT_GetAllDungeons) - a forward declaration here,
// since the definition is physically located later in the file but is
// already called from here (GrantSharedKillCredit) and from the main kill
// handler further down.

static void GrantSharedKillCredit(Player* member, uint32 killedEntry)
{
    uint32 guid = GetGuid(member);
    KTStats& mstats = GetOrLoadStats(member);

    // Daily quota
    uint32 today = CurrentDayKey();
    auto dayIt = g_dailyDayKey.find(guid);
    if (dayIt == g_dailyDayKey.end() || dayIt->second != today)
    {
        mstats.dailyKills = 0;
        g_dailyDayKey[guid] = today;
    }
    ++mstats.dailyKills;
    CharacterDatabase.Execute(
        "UPDATE mq_kill_stats SET daily_kills = IF(daily_date = CURDATE(), daily_kills + 1, 1), "
        "daily_date = CURDATE() WHERE guid = {}", guid);
    GrantDailyXpBuffIfReached(member, mstats, guid);

    // Daily contract
    CheckContractCompletion(member, mstats, killedEntry, guid);

    // Weekly bonus contract
    CheckWeeklyContractCompletion(member, killedEntry, guid);

    // Cleared dungeon/raid - NOT tied to isBoss (rank=WORLDBOSS), since the
    // final bosses of regular 5-mans usually have rank=ELITE, not
    // WORLDBOSS - checked against a separate "final bosses" list inside
    // CheckNewDungeonClear itself.
    CheckNewDungeonClear(member, mstats, killedEntry);
}

// Called ONLY for a boss (isBoss is checked before calling) - separate from
// GrantSharedKillCredit, since unique-boss credit isn't tied to "the day".
static void GrantSharedBossCredit(Player* member, KTStats& mstats, uint32 killedEntry)
{
    CheckNewBossKill(member, mstats, killedEntry, true);
}

// Grants daily quota/contract/boss credit/elite credit to all living
// members of the killer's party who are currently online and within the
// same range used for splitting kill XP (MaxGroupXPDistance, default 74
// yards) - the same threshold the server itself uses to decide "who earned
// credit for this kill".
static void ShareGroupKillCredit(Player* killer, uint32 killedEntry, bool isBoss, bool isEliteOrRare)
{
    Group* group = killer->GetGroup();
    if (!group)
        return;

    float range = sWorld->getFloatConfig(CONFIG_GROUP_XP_DISTANCE);

    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* member = itr->GetSource();
        if (!member || member == killer || !member->IsInWorld() || !member->IsAlive())
            continue;
        if (member->GetMapId() != killer->GetMapId() || member->GetDistance(killer) > range)
            continue;

        GrantSharedKillCredit(member, killedEntry);
        if (isBoss)
        {
            KTStats& mstats = GetOrLoadStats(member);
            GrantSharedBossCredit(member, mstats, killedEntry);
        }
        if (isEliteOrRare)
            GrantEliteCreditToPlayer(member);

    }
}

// ---- Live leaderboard overtake notifications ----

static void CheckOvertakeNotification(Player* player, uint32 newTotal)
{
    for (auto const& [sessGuid, session] : sWorldSessionMgr->GetAllSessions())
    {
        Player* other = session ? session->GetPlayer() : nullptr;
        if (!other || other == player || !other->IsInWorld())
            continue;

        KTStats const& otherStats = GetOrLoadStats(other);
        // An overtake is detected exactly at the moment of crossing, from
        // this kill: "before this kill I had <= them, now I'm > them".
        if (newTotal - 1 > otherStats.total || newTotal <= otherStats.total)
            continue;

        std::string msg = player->GetName() + " caught up to and passed " + other->GetName() + " in total kills!";
        ChatHandler(player->GetSession()).PSendSysMessage("|cff00ccff{}|r", msg);
        ChatHandler(other->GetSession()).PSendSysMessage("|cff00ccff{}|r", msg);
    }
}

// ---- HUD addon (KillTrackerHUD) ----
//
// HISTORY: this used to be a function called KT_SendAddonUpdate that sent
// the client a RAW CHAT_MSG_ADDON packet (built manually via
// ChatHandler::BuildChatPacket with a homemade prefix). On the client
// (CrossOver/Wine on Apple Silicon), handling that packet - BEFORE any Lua
// even ran, at the client's native level - caused a reliable ERROR #134
// "Fatal Condition" crash exactly when the packet was sent (entering the
// world / first mob kill), even with no client addon installed at all.
//
// NEW APPROACH (v2): instead of a homemade addon packet - a plain system
// message via ChatHandler(...).PSendSysMessage(...) - the same call this
// entire module already uses hundreds of times to message the player
// successfully (nothing new or risky at the protocol level).
// The line starts with a special marker, KT_HUD_MARKER, which the client
// addon catches via ChatFrame_AddMessageEventFilter("CHAT_MSG_SYSTEM", ...)
// - a standard, documented Blizzard API that addons have used for years to
// hide system messages from chat (the same mechanism used by login-spam
// filters) - and returns true so the player never sees this line at all.
// No raw packets, no custom protocol whatsoever.
// Plain printable ASCII text - no control bytes or non-standard content in
// the chat message (non-standard content was exactly what caused the
// previous crash, so this is deliberately the simplest and safest option
// possible).
static char const* KT_HUD_MARKER          = "##KTHUD##";
static char const* KT_HUD_CONTRACT_MARKER = "##KTCONTRACT##";
static char const* KT_HUD_BOARD_MARKER    = "##KTBOARD##";
static char const* KT_HUD_WEEKLY_MARKER   = "##KTWEEKLY##";

// Three payload lines (stats/contract/leaderboard) - all via the same
// proven path (PSendSysMessage); the client addon hides them from chat by
// their marker and sorts them into the HUD panel's tabs.
static void KT_SendHudUpdate(Player* player)
{
    KTStats const& stats = GetOrLoadStats(player);

    uint32 tier = std::min<uint32>(stats.total / KT_MILESTONE_STEP, KT_MILESTONE_MAX_TIER);
    uint32 remainToNextTier = (tier >= KT_MILESTONE_MAX_TIER) ? 0 : ((tier + 1) * KT_MILESTONE_STEP - stats.total);

    // Progress within the CURRENT tier (0-100) - for a visual progress bar
    // in the HUD (in addition to/instead of the "Next tier in: N" text).
    uint32 tierProgressPct = (tier >= KT_MILESTONE_MAX_TIER) ? 100
        : ((stats.total - tier * KT_MILESTONE_STEP) * 100 / KT_MILESTONE_STEP);

    uint32 bossPct = std::min<uint32>(stats.uniqueBossesKilled, KT_BOSS_MAX_UNIQUE) * KT_BOSS_STAT_PCT_PER_BOSS;

    uint32 eliteTier = std::min<uint32>(stats.elite / KT_ELITE_TIER_STEP, KT_ELITE_MAX_TIER);
    uint32 elitePct = eliteTier * KT_ELITE_STAT_PCT_PER_TIER;
    uint32 eliteRemain = (eliteTier >= KT_ELITE_MAX_TIER) ? 0 : ((eliteTier + 1) * KT_ELITE_TIER_STEP - stats.elite);
    uint32 eliteProgressPct = (eliteTier >= KT_ELITE_MAX_TIER) ? 100
        : ((stats.elite - eliteTier * KT_ELITE_TIER_STEP) * 100 / KT_ELITE_TIER_STEP);

    // "Master's Gift" (mod-missed-quests) - the completed-quest counter is
    // the built-in Player::GetRewardedQuestCount() (no include from another
    // module needed), the tier/step are duplicated constants
    // KT_QUEST_MILESTONE_STEP/MAX_TIER (see the comment in kill_milestones.h).
    uint32 questCompleted = uint32(player->GetRewardedQuestCount());
    uint32 questTier = std::min<uint32>(questCompleted / KT_QUEST_MILESTONE_STEP, KT_QUEST_MILESTONE_MAX_TIER);
    uint32 questRemain = (questTier >= KT_QUEST_MILESTONE_MAX_TIER) ? 0
        : ((questTier + 1) * KT_QUEST_MILESTONE_STEP - questCompleted);
    uint32 questProgressPct = (questTier >= KT_QUEST_MILESTONE_MAX_TIER) ? 100
        : ((questCompleted - questTier * KT_QUEST_MILESTONE_STEP) * 100 / KT_QUEST_MILESTONE_STEP);

    uint32 dungeonPct = std::min<uint32>(stats.uniqueDungeonsCleared, KT_DUNGEON_MAX_UNIQUE) * KT_DUNGEON_STAT_PCT_PER_DUNGEON;

    // Aggregated totals for the HUD's [Bonuses] tab - NOT new buff sources,
    // just a sum of already-granted bonuses (all the numbers above, or the
    // same eliteTier) grouped by WHICH stat they affect, not by WHERE they
    // came from:
    //   questPct - "Master's Gift" (mod-missed-quests), computed from the
    //     same questTier already above (Player::GetRewardedQuestCount() live).
    //   allStatsPct - bosses + dungeons + elite(stat) + quests (everything that
    //     SPELL_AURA_MOD_TOTAL_STAT_PERCENTAGE).
    //   hitPct/spellHitPct - the same eliteTier as elitePct above, just a
    //     different coefficient (KT_ELITE_HIT_PCT_PER_TIER) - melee+ranged
    //     together via one aura (54), spell separately (55), see GrantEliteAccuracyBonusIfReached.
    //   xpBonusPct - daily quota (SPELL_AURA_MOD_XP_PCT).
    uint32 questPct = questTier * KT_QUEST_MILESTONE_PERCENT;
    uint32 allStatsPct = bossPct + dungeonPct + elitePct + questPct;
    uint32 hitPct = eliteTier * KT_ELITE_HIT_PCT_PER_TIER;
    uint32 spellHitPct = hitPct; // same eliteTier - a separate field in case the sources ever diverge
    uint32 xpBonusPct = std::min<uint32>(stats.dailyCompletions, KT_DAILY_MAX_COMPLETIONS) * KT_DAILY_XP_PCT_PER_COMPLETION;

    // mod-multi-trainer: the user explicitly asked for stat NUMBERS, not
    // ability names ("WHAT DO I NEED SKILL NAME TEXT FOR"). The correct
    // source is NOT the arbitrary g_classPassive (no unified %-value system
    // there), but the multi-trainer's own tier system, "Master's Gift"
    // (20/40/60 TOTAL abilities learned through it = 3 tiers with ready-made
    // %-bonuses to specific stats - attack power, crit, speed, hit chance,
    // dodge, health, armor, all stats).
    std::string passiveNames = MT_GetMilestoneBonusSummary(player);

    // "Milestone: N enemies killed" - ready-made text with the bonus for the
    // kill-tracker's OWN tier (KT_ARMOR_PER_TIER/KT_ATTACK_POWER_PER_TIER/
    // KT_HASTE_PCT_PER_TIER * tier) - this was already computed and granted
    // (learnSpell) as always, but never made it onto the HUD's [Bonuses] tab.
    std::string killTierSummary = (tier > 0)
        ? Acore::StringFormat("tier {}: +{} armor, +{} attack power (melee/ranged/spell), +{}% attack speed",
              tier, tier * KT_ARMOR_PER_TIER, tier * KT_ATTACK_POWER_PER_TIER, tier * KT_HASTE_PCT_PER_TIER)
        : "";

    // Skills/professions/weapons - sum of "+1 per 10 points" across all
    // counted skills (KT_GetSkillBonusPoints, kill_milestones.h) - exactly
    // this amount is added to EACH of the 5 stats directly
    // (KT_ApplySkillStatBonus, WITHOUT spell_dbc - see the comment there).
    uint32 skillBonusPoints = KT_GetSkillBonusPoints(player);

    // Protocol version "9" - added eliteProgressPct/questProgressPct at the
    // end (server and client are always updated together, so a strict
    // "version == 9" check on the client is deliberate, as with versions "1"-"8").
    std::string body = Acore::StringFormat("9|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}",
        stats.total, tier, KT_MILESTONE_MAX_TIER, remainToNextTier, stats.streak, stats.bestStreak,
        stats.dailyKills, KT_DAILY_NORM, stats.dailyCompletions, stats.uniqueBossesKilled, bossPct,
        stats.elite, elitePct, eliteRemain, questCompleted, questTier, questRemain,
        stats.uniqueDungeonsCleared, dungeonPct,
        allStatsPct, hitPct, spellHitPct, xpBonusPct, killTierSummary, skillBonusPoints, passiveNames,
        tierProgressPct, eliteProgressPct, questProgressPct);

    ChatHandler(player->GetSession()).PSendSysMessage("{}{}", KT_HUD_MARKER, body);

    // Daily contract: hasContract|completed|name|location|X|Y|rewardXP|
    // secondsUntilReset. X/Y are already in client map format (0-100%),
    // ready for SlashCmdList["TOMTOM_WAY"] with no further conversion.
    // Version "2" - added secondsUntilReset at the end.
    KTDailyContract const& contract = KT_GetOrGenerateContract(player);
    float mapX = 0.0f, mapY = 0.0f;
    if (contract.hasContract)
        KT_GetContractMapCoords(contract, mapX, mapY);

    std::string contractBody = Acore::StringFormat("2|{}|{}|{}|{}|{:.2f}|{:.2f}|{}|{}",
        contract.hasContract ? 1 : 0, contract.completed ? 1 : 0,
        contract.creatureName, contract.areaName, mapX, mapY, contract.rewardXp,
        SecondsUntilNextMidnight());

    ChatHandler(player->GetSession()).PSendSysMessage("{}{}", KT_HUD_CONTRACT_MARKER, contractBody);

    // Leaderboard: top-5, to keep the line compact (every kill resends it).
    std::vector<KTLeaderboardEntry> board = KT_GetLeaderboard(5);
    std::string boardBody = "1";
    for (KTLeaderboardEntry const& entry : board)
        boardBody += Acore::StringFormat("|{}|{}", entry.name, entry.totalKills);

    ChatHandler(player->GetSession()).PSendSysMessage("{}{}", KT_HUD_BOARD_MARKER, boardBody);

    // Weekly bonus contract: hasContract|completed|name|continent|location|
    // isDungeon(0/1)|pinAvailable(0/1)|X|Y|rewardXP|secondsUntilReset.
    // pinAvailable is computed the same way as HandleBossItemClick in
    // npc_kill_tracker.cpp - comparing the player's CURRENT map/zone (at
    // send time) against the contract's saved own- or entrance-pin; if
    // neither matches, no pin is shown (for a dungeon boss the player
    // hasn't entered yet, the client is explicitly told "boss in dungeon
    // <name>").
    uint32 curMapId = player->GetMapId();
    uint32 curZoneId = player->GetZoneId();
    bool weeklyPinAvailable = false;
    float weeklyPinX = 0.0f, weeklyPinY = 0.0f;
    if (weekly.hasContract)
    {
        if (weekly.hasOwnPin && curMapId == weekly.ownMapId && curZoneId == weekly.ownZoneId)
        {
            weeklyPinAvailable = true;
            weeklyPinX = weekly.ownPinX; weeklyPinY = weekly.ownPinY;
        }
        else if (weekly.hasEntrancePin && curMapId == weekly.entranceMapId && curZoneId == weekly.entranceZoneId)
        {
            weeklyPinAvailable = true;
            weeklyPinX = weekly.entrancePinX; weeklyPinY = weekly.entrancePinY;
        }
    }

    std::string weeklyBody = Acore::StringFormat("1|{}|{}|{}|{}|{}|{}|{}|{:.2f}|{:.2f}|{}|{}",
        weekly.hasContract ? 1 : 0, weekly.completed ? 1 : 0, weekly.creatureName,
        weekly.continent, weekly.location, weekly.isDungeon ? 1 : 0,
        weeklyPinAvailable ? 1 : 0, weeklyPinX, weeklyPinY, weekly.rewardXp,
        SecondsUntilNextWeekReset());

    ChatHandler(player->GetSession()).PSendSysMessage("{}{}", KT_HUD_WEEKLY_MARKER, weeklyBody);
}

// A separate, VISIBLE (not hidden) message - only on login, not after every
// kill (that would spam), so the player immediately sees that a daily
// contract is available and not yet completed.
static void KT_NotifyContractIfAvailable(Player* player)
{
    KTDailyContract const& contract = KT_GetOrGenerateContract(player);
    if (contract.hasContract && !contract.completed)
    {
        ChatHandler(player->GetSession()).PSendSysMessage(
            "|cffffd700Daily contract:|r kill |cffff8000{}|r ({}) - reward {} XP + {} gold. "
            "See the \"Contract\" tab in KillTrackerHUD or the NPC menu.",
            contract.creatureName, contract.areaName, contract.rewardXp, KT_CONTRACT_GOLD_REWARD);
    }
}

// ---- Public helpers for npc_kill_tracker.cpp ----

KTStats const& KT_GetStats(Player* player)
{
    return GetOrLoadStats(player);
}

std::vector<KTCreatureKillEntry> KT_GetTopCreatures(Player* player, uint32 limit)
{
    std::vector<KTCreatureKillEntry> out;
    uint32 guid = GetGuid(player);

    QueryResult result = CharacterDatabase.Query(
        "SELECT creature_entry, kills FROM mq_kill_stats_by_creature WHERE guid = {} "
        "ORDER BY kills DESC LIMIT {}", guid, limit);
    if (!result)
        return out;

    do
    {
        Field* f = result->Fetch();
        KTCreatureKillEntry entry;
        entry.creatureEntry = f[0].Get<uint32>();
        entry.kills = f[1].Get<uint32>();
        entry.name = "Unknown";
        if (CreatureTemplate const* ct = sObjectMgr->GetCreatureTemplate(entry.creatureEntry))
            entry.name = ct->Name;
        out.push_back(std::move(entry));
    } while (result->NextRow());

    return out;
}

std::vector<KTCreatureKillEntry> KT_GetBossKills(Player* player)
{
    std::vector<KTCreatureKillEntry> out;
    uint32 guid = GetGuid(player);

    // There's no rank column in CharacterDB, so all of the player's personal
    // records are pulled and filtered in C++ by creature_template.rank
    // (few records per player - dozens/hundreds of types, not millions).
    QueryResult result = CharacterDatabase.Query(
        "SELECT creature_entry, kills FROM mq_kill_stats_by_creature WHERE guid = {} ORDER BY kills DESC", guid);
    if (!result)
        return out;

    do
    {
        Field* f = result->Fetch();
        uint32 entry = f[0].Get<uint32>();
        CreatureTemplate const* ct = sObjectMgr->GetCreatureTemplate(entry);
        if (!ct || ct->rank != uint32(CREATURE_ELITE_WORLDBOSS))
            continue;

        KTCreatureKillEntry e;
        e.creatureEntry = entry;
        e.kills = f[1].Get<uint32>();
        e.name = ct->Name;
        out.push_back(std::move(e));
    } while (result->NextRow());

    return out;
}

std::vector<KTCreatureKillEntry> KT_GetEliteRareKills(Player* player)
{
    std::vector<KTCreatureKillEntry> out;
    uint32 guid = GetGuid(player);

    // Same approach as KT_GetBossKills - pull all personal records and
    // filter in C++ by rank (ELITE/RAREELITE/RARE, i.e. NOT NORMAL and NOT
    // WORLDBOSS - bosses have their own list via KT_GetBossKills).
    QueryResult result = CharacterDatabase.Query(
        "SELECT creature_entry, kills FROM mq_kill_stats_by_creature WHERE guid = {} ORDER BY kills DESC", guid);
    if (!result)
        return out;

    do
    {
        Field* f = result->Fetch();
        uint32 entry = f[0].Get<uint32>();
        CreatureTemplate const* ct = sObjectMgr->GetCreatureTemplate(entry);
        if (!ct || ct->rank == uint32(CREATURE_ELITE_NORMAL) || ct->rank == uint32(CREATURE_ELITE_WORLDBOSS))
            continue;

        KTCreatureKillEntry e;
        e.creatureEntry = entry;
        e.kills = f[1].Get<uint32>();
        e.name = ct->Name;
        out.push_back(std::move(e));
    } while (result->NextRow());

    return out;
}

// Continent name from mapId - a fixed mapping for 3.3.5a (0=Eastern
// Kingdoms, 1=Kalimdor, 530=Outland, 571=Northrend). Instances have their
// own mapId and don't reach here - callers substitute "Dungeons" instead of
// calling this function for those.
static std::string ContinentNameForMap(uint32 mapId)
{
    switch (mapId)
    {
        case 0:   return "Eastern Kingdoms";
        case 1:   return "Kalimdor";
        case 530: return "Outland";
        case 571: return "Northrend";
        default:  return "Other";
    }
}

// Fixed sort order for continents/the "Dungeons" category in the
// [Boss Bonuses] menu - independent of the data, always stable.
static uint32 ContinentSortRank(std::string const& continent)
{
    if (continent == "Eastern Kingdoms") return 0;
    if (continent == "Kalimdor")          return 1;
    if (continent == "Outland")           return 2;
    if (continent == "Northrend")         return 3;
    if (continent == "Dungeons")          return 4;
    return 5;
}

// Looks for the OUTSIDE exit of instance dungeonMapId via
// areatrigger_teleport (trigger ID -> target=instance) + areatrigger (the
// same ID -> the point on the continent where you enter from) - the same ID
// link used by the client/server itself for dungeon portals. Returns false
// if this map has no known outside entrance at all (e.g. some open-world
// raids).
static bool FindDungeonEntrance(uint32 dungeonMapId, uint32& outMapId, float& outX, float& outY)
{
    QueryResult trig = WorldDatabase.Query(
        "SELECT ID FROM areatrigger_teleport WHERE target_map = {} LIMIT 1", dungeonMapId);
    if (!trig)
        return false;

    uint32 triggerId = trig->Fetch()[0].Get<uint32>();

    QueryResult at = WorldDatabase.Query(
        "SELECT map, x, y FROM areatrigger WHERE entry = {} LIMIT 1", triggerId);
    if (!at)
        return false;

    Field* f = at->Fetch();
    outMapId = f[0].Get<uint32>();
    outX = f[1].Get<float>();
    outY = f[2].Get<float>();
    return true;
}

std::vector<KTBossEntry> KT_GetAllBossLocations()
{
    std::vector<KTBossEntry> out;

    // One representative spawn point per entry (GROUP BY without an
    // aggregate - MySQL returns an arbitrary row from the group, which is
    // fine for our purpose of "where to even look for this boss"). rank=
    // WORLDBOSS is filtered via the join right away - otherwise millions of
    // spawns of every creature would need to be pulled.
    // MIN(guid) subquery instead of "GROUP BY c.id" - doesn't rely on the
    // (not always enabled) permissive ONLY_FULL_GROUP_BY mode, and gives a
    // deterministic (lowest guid) single spawn per entry.
    QueryResult spawns = WorldDatabase.Query(
        "SELECT c.id, c.map, c.position_x, c.position_y, c.position_z, ct.Name, ct.minlevel, ct.maxlevel "
        "FROM creature c JOIN creature_template ct ON ct.entry = c.id "
        "WHERE ct.rank = {} AND c.guid = (SELECT MIN(c2.guid) FROM creature c2 WHERE c2.id = c.id)",
        uint32(CREATURE_ELITE_WORLDBOSS));
    if (!spawns)
        return out;

    // Dungeon-entrance cache (mapId -> found), so the same query isn't hit
    // separately for every boss of the same instance (a raid can have
    // several bosses on one mapId).
    std::map<uint32, std::pair<bool, std::pair<float, float>>> entranceCache; // dungeonMapId -> {found, {x,y}}
    std::map<uint32, uint32> entranceZoneCache; // dungeonMapId -> outdoor zoneId
    std::map<uint32, uint32> entranceOutMapCache; // dungeonMapId -> outdoor mapId (continent where the entrance itself is)

    do
    {
        Field* f = spawns->Fetch();
        KTBossEntry e;
        e.creatureEntry = f[0].Get<uint32>();
        uint32 mapId    = f[1].Get<uint32>();
        float x = f[2].Get<float>();
        float y = f[3].Get<float>();
        float z = f[4].Get<float>();
        e.name = f[5].Get<std::string>();
        uint32 minLevel = f[6].Get<uint8>();
        uint32 maxLevel = f[7].Get<uint8>();
        e.level = (minLevel == maxLevel) ? std::to_string(minLevel)
            : (std::to_string(minLevel) + "-" + std::to_string(maxLevel));
        e.minLevel = minLevel;

        // We do NOT trust the creature.zoneId column - on many spawns it was
        // never filled in (stayed 0), which used to make half the bosses show
        // "unknown zone". The zone is computed LIVE from the real world
        // coordinates (the same method as .gps/PickContractTarget) - always
        // correct, regardless of that DB column's state.
        uint32 zoneId = sMapMgr->GetZoneId(PHASEMASK_NORMAL, mapId, x, y, z);

        MapEntry const* mapEntry = sMapStore.LookupEntry(mapId);
        bool instanceable = mapEntry && mapEntry->Instanceable();

        if (instanceable)
        {
            e.isDungeon = true;
            e.continent = "Dungeons";
            e.location  = mapEntry->name[LOCALE_enUS];

            // "On-site" pin - the boss's own coordinates inside the instance.
            // Fires only when the player is ALREADY in that same instance
            // (checked via GetMapId()/GetZoneId() in npc_kill_tracker.cpp).
            e.hasOwnPin = true;
            e.ownMapId = mapId;
            e.ownZoneId = zoneId;
            e.ownPinX = x; e.ownPinY = y;
            Map2ZoneCoordinates(e.ownPinX, e.ownPinY, zoneId);

            // ENTRANCE pin - the OUTSIDE coordinates used to enter the instance.
            auto cacheIt = entranceCache.find(mapId);
            if (cacheIt == entranceCache.end())
            {
                uint32 outMapId = 0; float outX = 0.0f, outY = 0.0f;
                bool found = FindDungeonEntrance(mapId, outMapId, outX, outY);
                if (found)
                {
                    entranceOutMapCache[mapId] = outMapId;
                    entranceZoneCache[mapId] = sMapMgr->GetZoneId(PHASEMASK_NORMAL, outMapId, outX, outY, 0.0f);
                }
                cacheIt = entranceCache.emplace(mapId, std::make_pair(found, std::make_pair(outX, outY))).first;
            }

            if (cacheIt->second.first)
            {
                float entX = cacheIt->second.second.first;
                float entY = cacheIt->second.second.second;
                uint32 entZoneId = entranceZoneCache[mapId];
                Map2ZoneCoordinates(entX, entY, entZoneId);

                e.hasEntrancePin = true;
                e.entranceMapId = entranceOutMapCache[mapId];
                e.entranceZoneId = entZoneId;
                e.entrancePinX = entX; e.entrancePinY = entY;
            }
        }
        else
        {
            e.isDungeon = false;
            e.continent = ContinentNameForMap(mapId);

            e.location = "Unknown Zone";
            if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(zoneId))
                e.location = area->area_name[LOCALE_enUS];

            e.hasOwnPin = true;
            e.ownMapId = mapId;
            e.ownZoneId = zoneId;
            e.ownPinX = x; e.ownPinY = y;
            Map2ZoneCoordinates(e.ownPinX, e.ownPinY, zoneId);
        }

        out.push_back(std::move(e));
    } while (spawns->NextRow());

    std::sort(out.begin(), out.end(), [](KTBossEntry const& a, KTBossEntry const& b)
    {
        uint32 ra = ContinentSortRank(a.continent), rb = ContinentSortRank(b.continent);
        if (ra != rb) return ra < rb;
        if (a.location != b.location) return a.location < b.location;
        return a.name < b.name;
    });

    return out;
}

// ---- Cleared dungeons/raids ----
//
// Source of truth - the world DB table `instance_encounters` (the same one
// that drives the standard LFG reward system): a row with lastEncounterDungeon
// != 0 marks that creditEntry is the BOSS whose death Blizzard considers
// "this dungeon/wing cleared". Only creditType=0 (kill credit) is counted -
// see the comment near KT_DUNGEON_* in kill_milestones.h regarding the few
// instances where Blizzard counts completion differently (spell cast/event).
//
// IMPORTANT: creditEntry (the boss entry itself) IS the credit key
// (mq_dungeon_credit.credit_entry), NOT mapId. Several DIFFERENT wings can
// live on the same mapId (Scarlet Monastery: Graveyard/Library/Armory/
// Cathedral - 4 different bosses on mapId=189; Maraudon: Orange/Purple
// Crystals/Pristine Waters - 3 different bosses on mapId=349, etc. -
// verified directly against instance_encounters.sql) - if mapId were the
// key, all wings but one would be lost. Normal and heroic versions of the
// SAME boss have DIFFERENT lastEncounterDungeon values but the same
// creditEntry - so they naturally collapse into ONE entry with no extra logic.
static std::set<uint32> const& GetTrackedFinalBosses()
{
    static std::set<uint32> cache;
    static bool loaded = false;
    if (!loaded)
    {
        loaded = true;
        if (QueryResult result = WorldDatabase.Query(
                "SELECT DISTINCT creditEntry FROM instance_encounters "
                "WHERE lastEncounterDungeon <> 0 AND creditType = 0"))
        {
            do
            {
                cache.insert(result->Fetch()[0].Get<uint32>());
            } while (result->NextRow());
        }
    }
    return cache;
}

std::vector<KTDungeonEntry> KT_GetAllDungeons()
{
    std::vector<KTDungeonEntry> out;

    // creditEntry and lastEncounterDungeon TOGETHER - deduplicate by
    // creditEntry (see the KTDungeonEntry comment in the .h), not by
    // mapId/lastEncounterDungeon, or different wings on the same mapId
    // would merge into one entry.
        "SELECT DISTINCT creditEntry, lastEncounterDungeon FROM instance_encounters "
        "WHERE lastEncounterDungeon <> 0 AND creditType = 0");
    if (!result)
        return out;

    std::set<uint32> seenCreditEntries;
    do
    {
        Field* f = result->Fetch();
        uint32 creditEntry = f[0].Get<uint32>();
        uint32 lfgId = f[1].Get<uint32>();

        if (!seenCreditEntries.insert(creditEntry).second)
            continue; // this boss was already added (normal/heroic version)

        LFGDungeonEntry const* lfg = sLFGDungeonStore.LookupEntry(lfgId);
        if (!lfg)
            continue;

        KTDungeonEntry e;
        e.creditEntry = creditEntry;
        e.mapId = lfg->MapID;
        e.name = lfg->Name[0]; // enUS
        e.minLevel = lfg->MinLevel;
        e.maxLevel = lfg->MaxLevel;
        e.isRaid = (lfg->TypeID == 2); // LFGDungeonEntry::TypeID: 1=dungeon, 2=raid (lfg::LFG_TYPE_RAID)
        e.lfgDungeonId = lfgId; // the same ID just used for sLFGDungeonStore.LookupEntry() above

        // Continent is resolved via the instance's real EXTERNAL entrance -
        // the same FindDungeonEntrance already used by the [Boss Bonuses]
        // list for the entrance pin. More reliable than hardcoding "which
        // dungeon is on which continent".
        uint32 outMapId = 0; float outX = 0.0f, outY = 0.0f;
        e.continent = FindDungeonEntrance(e.mapId, outMapId, outX, outY)
            ? ContinentNameForMap(outMapId) : "Other";

        out.push_back(std::move(e));
    } while (result->NextRow());

    std::sort(out.begin(), out.end(), [](KTDungeonEntry const& a, KTDungeonEntry const& b)
    {
        uint32 ra = ContinentSortRank(a.continent), rb = ContinentSortRank(b.continent);
        if (ra != rb) return ra < rb;
        if (a.name != b.name) return a.name < b.name;
        return a.creditEntry < b.creditEntry; // stable ordering for same-named wings (e.g. heroic pairs)
    });

    return out;
}

std::vector<uint32> KT_GetCreditedDungeonEntries(Player* player)
{
    std::vector<uint32> out;
    uint32 guid = GetGuid(player);

    QueryResult result = CharacterDatabase.Query(
        "SELECT credit_entry FROM mq_dungeon_credit WHERE guid = {}", guid);
    if (!result)
        return out;

    do
    {
        out.push_back(result->Fetch()[0].Get<uint32>());
    } while (result->NextRow());

    return out;
}

// ---- Cleared-dungeon bonus (same approach as the boss bonus) ----

static void GrantDungeonBonusIfReached(Player* player, uint32 uniqueDungeonsCleared, uint32 tier, std::string const& justClearedName)
{
    if (uniqueDungeonsCleared < tier)
        return;

    uint32 spellId = KT_DUNGEON_BONUS_BASE_SPELL + (tier - 1);
    if (player->HasSpell(spellId))
        return;

    player->learnSpell(spellId);

    ChatHandler(player->GetSession()).PSendSysMessage(
        "|cff40c0ffDungeon cleared:|r \"{}\" - permanent bonus is now |cffffd700+{}% to all stats|r "
        "(total dungeons cleared: {})!",
        justClearedName, tier * KT_DUNGEON_STAT_PCT_PER_DUNGEON, uniqueDungeonsCleared);
}

static void CheckDungeonBonusMilestones(Player* player, uint32 uniqueDungeonsCleared, std::string const& justClearedName)
{
    for (uint32 tier = 1; tier <= KT_DUNGEON_MAX_UNIQUE; ++tier)
        GrantDungeonBonusIfReached(player, uniqueDungeonsCleared, tier, justClearedName);
}

// Called on EVERY kill (both for the killer and party members in
// GrantSharedKillCredit below), but only actually does anything when
// killedEntry matches one of the "final bosses" in GetTrackedFinalBosses() -
// a cheap set::count check, no DB query for the vast majority of calls
// (regular mobs never match).
static void CheckNewDungeonClear(Player* player, KTStats& stats, uint32 killedEntry)
{
    if (!GetTrackedFinalBosses().count(killedEntry))
        return;

    uint32 creditEntry = killedEntry; // the boss entry itself IS the credit key (see KTDungeonEntry)
    uint32 guid = GetGuid(player);

    if (QueryResult already = CharacterDatabase.Query(
            "SELECT 1 FROM mq_dungeon_credit WHERE guid = {} AND credit_entry = {}", guid, creditEntry))
        return; // credit for this dungeon/wing was already granted earlier

    CharacterDatabase.Execute(
        "INSERT INTO mq_dungeon_credit (guid, credit_entry) VALUES ({}, {})", guid, creditEntry);

    ++stats.uniqueDungeonsCleared;
    CharacterDatabase.Execute(
        "UPDATE mq_kill_stats SET dungeons_cleared = {} WHERE guid = {}", stats.uniqueDungeonsCleared, guid);

    std::string name = "the dungeon";
    for (KTDungeonEntry const& d : KT_GetAllDungeons())
        if (d.creditEntry == creditEntry) { name = d.name; break; }

    CheckDungeonBonusMilestones(player, stats.uniqueDungeonsCleared, name);
}

// Removes bonus spells for tiers ABOVE newCount - used only by the manual
// toggle below (the automatic tracker via CheckNewDungeonClear never
// decreases the counter, so it never needs this function).
static void RemoveDungeonBonusAboveTier(Player* player, uint32 newCount)
{
    for (uint32 tier = newCount + 1; tier <= KT_DUNGEON_MAX_UNIQUE; ++tier)
    {
        uint32 spellId = KT_DUNGEON_BONUS_BASE_SPELL + (tier - 1);
        if (player->HasSpell(spellId))
            player->removeSpell(spellId, SPEC_MASK_ALL, false);
    }
}

// Syncs the dungeons_cleared counter with the ACTUAL row count in
// mq_dungeon_credit - a safeguard against drift (e.g. if credit rows ever
// need to be rewritten by a migration again, as happened during the switch
// to creditEntry - the counter and the rows themselves could drift apart if
// the player toggled something between builds). Called on every login
// (OnPlayerLogin), BEFORE CheckDungeonBonusMilestones. If the count
// decreases, it removes the excess tier bonus spells (RemoveDungeonBonusAboveTier);
// if it increases, CheckDungeonBonusMilestones (called right after) grants
// the missing ones itself.
static void ResyncDungeonCount(Player* player, KTStats& stats)
{
    uint32 guid = GetGuid(player);

    uint32 realCount = 0;
    if (QueryResult result = CharacterDatabase.Query(
            "SELECT COUNT(*) FROM mq_dungeon_credit WHERE guid = {}", guid))
        realCount = result->Fetch()[0].Get<uint32>();

    if (realCount == stats.uniqueDungeonsCleared)
        return;

    uint32 oldCount = stats.uniqueDungeonsCleared;
    stats.uniqueDungeonsCleared = realCount;
    CharacterDatabase.Execute(
        "UPDATE mq_kill_stats SET dungeons_cleared = {} WHERE guid = {}", realCount, guid);

    if (realCount < oldCount)
        RemoveDungeonBonusAboveTier(player, realCount);

    ChatHandler(player->GetSession()).PSendSysMessage(
        "|cff888888[Sync]|r Cleared dungeon count corrected: {} -> {} (based on the actual list in [Cleared Dungeons]).",
        oldCount, realCount);
}

// Manual dungeon credit toggle - see the comment in kill_milestones.h.
bool KT_ToggleDungeonCredit(Player* player, uint32 creditEntry)
{
    uint32 guid = GetGuid(player);
    KTStats& stats = GetOrLoadStats(player);

    bool hadCredit = CharacterDatabase.Query(
        "SELECT 1 FROM mq_dungeon_credit WHERE guid = {} AND credit_entry = {}", guid, creditEntry) != nullptr;

    std::string name = "the dungeon";
    for (KTDungeonEntry const& d : KT_GetAllDungeons())
        if (d.creditEntry == creditEntry) { name = d.name; break; }

    if (hadCredit)
    {
        CharacterDatabase.Execute(
            "DELETE FROM mq_dungeon_credit WHERE guid = {} AND credit_entry = {}", guid, creditEntry);

        if (stats.uniqueDungeonsCleared > 0)
            --stats.uniqueDungeonsCleared;
        CharacterDatabase.Execute(
            "UPDATE mq_kill_stats SET dungeons_cleared = {} WHERE guid = {}", stats.uniqueDungeonsCleared, guid);

        RemoveDungeonBonusAboveTier(player, stats.uniqueDungeonsCleared);

        ChatHandler(player->GetSession()).PSendSysMessage(
            "|cff8b0000Credit removed:|r \"{}\" no longer counts as cleared (total dungeons cleared: {}).",
            name, stats.uniqueDungeonsCleared);
        KT_ApplySkillStatBonus(player); // the dungeon % just changed - the skill overlay needs recomputing
        KT_SendHudUpdate(player); // live HUD addon update - without this the "Dungeons cleared" line would only update on relog/zone change
        return false;
    }

    CharacterDatabase.Execute(
        "INSERT IGNORE INTO mq_dungeon_credit (guid, credit_entry) VALUES ({}, {})", guid, creditEntry);

    ++stats.uniqueDungeonsCleared;
    CharacterDatabase.Execute(
        "UPDATE mq_kill_stats SET dungeons_cleared = {} WHERE guid = {}", stats.uniqueDungeonsCleared, guid);

    CheckDungeonBonusMilestones(player, stats.uniqueDungeonsCleared, name);
    KT_ApplySkillStatBonus(player); // same, other direction
    KT_SendHudUpdate(player); // live HUD addon update
    return true;
}

std::vector<uint32> KT_GetCreditedBossEntries(Player* player)
{
    std::vector<uint32> out;
    uint32 guid = GetGuid(player);

    QueryResult result = CharacterDatabase.Query(
        "SELECT creature_entry FROM mq_boss_credit WHERE guid = {}", guid);
    if (!result)
        return out;

    do
    {
        out.push_back(result->Fetch()[0].Get<uint32>());
    } while (result->NextRow());

    return out;
}

std::vector<KTLeaderboardEntry> KT_GetLeaderboard(uint32 limit)
{
    std::vector<KTLeaderboardEntry> out;

    QueryResult result = CharacterDatabase.Query(
        "SELECT c.name, k.total_kills FROM mq_kill_stats k JOIN characters c ON c.guid = k.guid "
        "ORDER BY k.total_kills DESC LIMIT {}", limit);
    if (!result)
        return out;

    do
    {
        Field* f = result->Fetch();
        KTLeaderboardEntry entry;
        entry.name = f[0].Get<std::string>();
        entry.totalKills = f[1].Get<uint32>();
        out.push_back(std::move(entry));
    } while (result->NextRow());

    return out;
}

void KT_ResetCache()
{
    g_stats.clear();
    g_contracts.clear();
    g_contractDayKey.clear();
    g_weeklyContracts.clear();
    g_weeklyWeekKey.clear();
}

// ---- Skill/profession/weapon proficiency bonus (kill_milestones.h has the
// full comment explaining why this is NOT done via spell_dbc) ----

// How much has already been applied (sum of bonusPoints at the last
// KT_ApplySkillStatBonus call) - needed to know the DELTA on a repeat call
// (HandleStatFlatModifier is "add/subtract", not "set an absolute value", so
// without this cache we'd either have to remove and reapply everything
// every time, or get duplicates on every call).
static std::map<uint32 /*guid*/, uint32> g_appliedSkillBonus;

static bool IsSkillCategoryCounted(int32 categoryId)
{
    return categoryId == SKILL_CATEGORY_WEAPON
        || categoryId == SKILL_CATEGORY_PROFESSION
        || categoryId == SKILL_CATEGORY_SECONDARY;
}

std::vector<KTSkillBonusEntry> KT_GetSkillBonusEntries(Player* player)
{
    std::vector<KTSkillBonusEntry> out;
    if (!player)
        return out;

    LocaleConstant locale = player->GetSession() ? player->GetSession()->GetSessionDbcLocale() : LOCALE_enUS;

    for (uint16 i = 0; i < PLAYER_MAX_SKILLS; ++i)
    {
        uint32 idField = player->GetUInt32Value(PLAYER_SKILL_INDEX(i));
        uint32 skillId = PAIR32_LOPART(idField);
        if (!skillId)
            continue;

        SkillLineEntry const* skillLine = sSkillLineStore.LookupEntry(skillId);
        if (!skillLine || !IsSkillCategoryCounted(skillLine->categoryId))
            continue;

        uint32 valField = player->GetUInt32Value(PLAYER_SKILL_VALUE_INDEX(i));
        uint16 value = uint16(PAIR32_LOPART(valField));
        uint16 maxValue = uint16(PAIR32_HIPART(valField));
        if (value == 0)
            continue; // "learned but 0 points" (happens for some skill-line records with no real progress)

        KTSkillBonusEntry entry;
        entry.skillId = skillId;
        entry.name = skillLine->name[locale] && *skillLine->name[locale] ? skillLine->name[locale] : skillLine->name[LOCALE_enUS];
        entry.value = value;
        entry.maxValue = maxValue;
        entry.bonusPoints = value / KT_SKILL_POINTS_PER_BONUS;
        out.push_back(entry);
    }

    std::sort(out.begin(), out.end(), [](KTSkillBonusEntry const& a, KTSkillBonusEntry const& b)
    {
        return a.name < b.name;
    });

    return out;
}

uint32 KT_GetSkillBonusPoints(Player* player)
{
    uint32 total = 0;
    for (KTSkillBonusEntry const& e : KT_GetSkillBonusEntries(player))
        total += e.bonusPoints;
    return total;
}

// IMPORTANT (per the user's request - "so it doesn't get multiplied"):
// the standard WoW stat formula (Unit::GetTotalStatValue, StatSystem.cpp):
//   value = ((base_value * base_pct) + total_value) * total_pct
// BOTH base_value AND total_value get MULTIPLIED by total_pct at the end -
// so a plain SPELL_AURA_MOD_STAT (like the previous version of this
// function, which went through HandleStatFlatModifier/TOTAL_VALUE - the
// same path this aura takes) would also get caught by that multiplier. As
// a result, on a character with a significant %-stat bonus (bosses/
// dungeons/elite/quests/Master's Gift) +602 points would show up as a much
// bigger gain than +602 - the formula correctly multiplied it, just like
// any regular item stat.
//
// The only way to avoid this multiplication on this core is to NOT
// participate in the standard stat pipeline at all: first let
// Player::UpdateStats() compute the "clean" value (with all %, WITHOUT our
// bonus), and only THEN apply the bonus ON TOP directly via SetStat() -
// already AFTER the total_pct multiplication, so the bonus itself is always
// exactly +602 (or whatever it is), independent of the rest of the %s.
// Derived stats (health/mana/armor/crit/attack power/spell power), which
// themselves depend on the primary stats, are recomputed ONE MORE TIME at
// the end - now from the bonus-"inflated" values, so the effect correctly
// propagates to them too (otherwise the bonus would only affect the number
// shown on the character panel, not the health/armor/attack power computed
// from it).
// A full idempotent recompute from scratch every time (not a delta from the
// cache, as before) - self-correcting even if called at an unusual time or
// repeatedly, and always gives the correct result regardless of what
// exactly changed elsewhere (the player doesn't care whether this is our
// call after a skill change or some unrelated side effect).
void KT_ApplySkillStatBonus(Player* player)
{
    if (!player)
        return;

    uint32 newTotal = KT_GetSkillBonusPoints(player);
    g_appliedSkillBonus[GetGuid(player)] = newTotal; // for reference/compat with KT_ClearSkillStatBonusCache only

    for (int32 stat = STAT_STRENGTH; stat < MAX_STATS; ++stat)
    {
        player->UpdateStats(Stats(stat)); // "clean" value (with all %, WITHOUT our bonus) - resets the previous overlay
        int32 pureValue = int32(player->GetStat(Stats(stat)));
        player->SetStat(Stats(stat), pureValue + int32(newTotal)); // apply the bonus AFTER the total_pct multiplication - no multiplying
    }

    // The same set of derived recalculations Player::UpdateStats(stat)
    // performs for each stat (StatSystem.cpp) - repeated here SEPARATELY
    // (without calling UpdateStats again, which would reset SetStat() back
    // to the clean value), so health/armor/crit/attack power/spell power
    // are computed from the ALREADY-inflated stats.
    player->UpdateShieldBlockValue();
    player->UpdateArmor();
    player->UpdateAllCritPercentages();
    player->UpdateDodgePercentage();
    player->UpdateMaxHealth();
    player->UpdateMaxPower(POWER_MANA);
    player->UpdateAllSpellCritChances();
    player->UpdateAttackPowerAndDamage(false);
    player->UpdateAttackPowerAndDamage(true);
    player->UpdateSpellDamageAndHealingBonus();
    player->UpdateManaRegen();
}

void KT_ClearSkillStatBonusCache(Player* player)
{
    g_appliedSkillBonus.erase(GetGuid(player));
}

static std::string const KT_TITLE_NAMES[KT_TITLE_COUNT] = {
    "Hunting Novice",          // tier 10
    "Tracker",                 // tier 20
    "Skilled Hunter",          // tier 30
    "Scourge of the Wilds",    // tier 40
    "Exterminator",            // tier 50
    "Legend of the Hunt",      // tier 60
    "Master of Prey",          // tier 70
    "Scourge of Azeroth",      // tier 80
    "Unbreakable Hunter",      // tier 90
    "Lord of the Hunt",        // tier 100
};

std::string const& KT_GetTitleName(uint32 titleIndex)
{
    static std::string const fallback = "???";
    if (titleIndex >= KT_TITLE_COUNT)
        return fallback;
    return KT_TITLE_NAMES[titleIndex];
}

class KillTrackerPlayerScript : public PlayerScript
{
public:
    KillTrackerPlayerScript()
        : PlayerScript("KillTrackerPlayerScript",
            { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_ON_CREATURE_KILL, PLAYERHOOK_ON_PLAYER_JUST_DIED,
              PLAYERHOOK_ON_PLAYER_COMPLETE_QUEST, PLAYERHOOK_ON_UPDATE_SKILL, PLAYERHOOK_ON_SET_SKILL }) { }

    void OnPlayerLogin(Player* player) override
    {
        KTStats& stats = GetOrLoadStats(player);
        CheckKillMilestones(player, stats); // safety check (e.g. after a constant change)
        CheckBossBonusMilestones(player, stats.uniqueBossesKilled, "");
        ResyncDungeonCount(player, stats); // fixes any drift between the counter and the real mq_dungeon_credit rows (e.g. after the creditEntry migration)
        CheckDungeonBonusMilestones(player, stats.uniqueDungeonsCleared, ""); // safety check (e.g. after a seed grant or constant change)
        CheckEliteTierMilestones(player, stats.elite); // safety check on every login - also retroactively picks up historical 125+ kills
        KT_ApplySkillStatBonus(player); // +1/10 skill points to all stats - full recompute on every login
        KT_SendHudUpdate(player); // initial state for the HUD addon (if installed) - now via a plain system message
        KT_NotifyContractIfAvailable(player); // visible daily contract reminder (login only)
        KT_NotifyWeeklyContractIfAvailable(player); // same for the weekly bonus contract
    }

    void OnPlayerLogout(Player* player) override
    {
        uint32 guid = GetGuid(player);
        g_stats.erase(guid); // few players, but clean up after ourselves
        g_contracts.erase(guid);
        g_dailyDayKey.erase(guid);
        g_contractDayKey.erase(guid);
        g_weeklyContracts.erase(guid);
        g_weeklyWeekKey.erase(guid);
        KT_ClearSkillStatBonusCache(player);
    }

    // Skills level up FAR more often than kills (every successful
    // craft/gather), so the bonus for them is only recomputed HERE (on a
    // skill change), not on every tick - KT_ApplySkillStatBonus itself
    // computes the delta and does nothing if the point total hasn't changed.
    void OnPlayerUpdateSkill(Player* player, uint32 /*skillId*/, uint32 /*value*/, uint32 /*max*/, uint32 /*step*/, uint32 /*newValue*/) override
    {
        KT_ApplySkillStatBonus(player);
        KT_SendHudUpdate(player);
    }

    void OnPlayerSetSkill(Player* player, uint32 /*skillId*/, uint32 /*value*/, uint32 /*max*/, uint32 /*step*/, uint32 /*newValue*/) override
    {
        KT_ApplySkillStatBonus(player);
        KT_SendHudUpdate(player);
    }

    // "Master's Gift" (quest tier) on the HUD addon's Stats tab is computed
    // LIVE from player->GetRewardedQuestCount() in KT_SendHudUpdate - without
    // this hook the HUD would only refresh on a mob kill, so a just-turned-in
    // quest would show up in the HUD late (only on the next kill/map change,
    // not immediately). The NPC already computes its own stats live on every
    // menu open - this only concerns the HUD.
    void OnPlayerCompleteQuest(Player* player, Quest const* /*quest*/) override
    {
        // Quest tier (mod-missed-quests) is a %-stat bonus, so the skill
        // overlay (KT_ApplySkillStatBonus) needs recomputing too - otherwise
        // the "clean" value it adds +N on top of would stay stale until the
        // next login/skill change.
        KT_ApplySkillStatBonus(player);
        KT_SendHudUpdate(player);
    }

    void OnPlayerJustDied(Player* player) override
    {
        KTStats& stats = GetOrLoadStats(player);
        if (stats.streak == 0)
            return;

        stats.streak = 0;
        CharacterDatabase.Execute("UPDATE mq_kill_stats SET streak_kills = 0 WHERE guid = {}", GetGuid(player));
    }

    void OnPlayerCreatureKill(Player* player, Creature* killed) override
    {
        KTStats& stats = GetOrLoadStats(player);
        ++stats.total;

        uint32 rank = killed->GetCreatureTemplate() ? killed->GetCreatureTemplate()->rank : uint32(CREATURE_ELITE_NORMAL);
        bool isBoss = (rank == CREATURE_ELITE_WORLDBOSS);

        bool isEliteOrRare = !isBoss && rank != CREATURE_ELITE_NORMAL;

        char const* bucket = "normal_kills";
        if (isBoss)
        {
            ++stats.boss;
            bucket = "boss_kills";
        }
        else if (isEliteOrRare)
        {
            ++stats.elite;
            bucket = "elite_kills";
            CheckEliteTierMilestones(player, stats.elite);
        }
        else
        {
            ++stats.normal;
        }

        uint32 guid = GetGuid(player);

        CharacterDatabase.Execute("UPDATE mq_kill_stats SET total_kills = total_kills + 1, {} = {} + 1 WHERE guid = {}",
            bucket, bucket, guid);

        // Kill streak (no death)
        ++stats.streak;
        stats.bestStreak = std::max(stats.bestStreak, stats.streak);
        CharacterDatabase.Execute("UPDATE mq_kill_stats SET streak_kills = {}, best_streak = {} WHERE guid = {}",
            stats.streak, stats.bestStreak, guid);
        GrantStreakBuffIfReached(player, stats.streak);

        // Personal per-creature-type stats
        uint32 entry = killed->GetEntry();
        CharacterDatabase.Execute(
            "INSERT INTO mq_kill_stats_by_creature (guid, creature_entry, kills) VALUES ({}, {}, 1) "
            "ON DUPLICATE KEY UPDATE kills = kills + 1", guid, entry);

        // Favorite farming zone
        TrackZoneKill(player, guid);

        // Daily quota + daily contract (same path used for party members -
        // see GrantSharedKillCredit/ShareGroupKillCredit below).
        GrantSharedKillCredit(player, entry);

        // Cosmetic trophies (stay personal - tied to the player's own tier)
        GrantTrophyIfReached(player, stats.total);

        CheckNewBossKill(player, stats, entry, isBoss);
        CheckKillMilestones(player, stats);
        CheckOvertakeNotification(player, stats.total);

        // Group credit: daily quota/contract/boss bonus/elite - for ALL
        // LIVING PARTY MEMBERS (except the killer - already counted above)
        // in XP-share range, not just the killing blow.
        ShareGroupKillCredit(player, entry, isBoss, isEliteOrRare);

        // The kill may have just granted a new %-tier (boss/elite/kill-
        // milestone) - the "clean" value the skill overlay adds onto needs
        // recomputing (see the comment near KT_ApplySkillStatBonus).
        KT_ApplySkillStatBonus(player);

        // HUD addon (if installed) - live update after every kill.
        KT_SendHudUpdate(player);
    }
};

void AddKillMilestonesScripts()
{
    new KillTrackerPlayerScript();
}
