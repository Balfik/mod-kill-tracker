#pragma once

#include "Define.h"

#include <ctime>
#include <string>
#include <vector>

class Player;

// Personal kill statistics for a player - kept in process memory
// (kill_milestones.cpp), persisted to mq_kill_stats. Shared struct so that
// npc_kill_tracker.cpp (the Status menu) can read the same data without
// duplicating DB queries.
struct KTStats
{
    uint32 total  = 0;
    uint32 normal = 0;
    uint32 elite  = 0;
    uint32 boss   = 0;

    // Consecutive kill streak without dying - resets to 0 on death (see
    // OnPlayerJustDied). bestStreak is the player's record, never decreases.
    uint32 streak     = 0;
    uint32 bestStreak = 0;

    // How many days ago the first record appeared - used to estimate the
    // pace ("kills/day") shown in the NPC's Status menu. Computed once on
    // load (not refreshed every second - a small error around midnight
    // doesn't matter).
    uint32 daysTracked = 0;

    // Number of DIFFERENT (unique) bosses the player has killed at least
    // once - source of the "% to all stats per new boss" bonus.
    uint32 uniqueBossesKilled = 0;

    // Number of DIFFERENT (unique) dungeons/raids cleared (mq_dungeon_credit) -
    // source of the "% to all stats per cleared dungeon" bonus.
    uint32 uniqueDungeonsCleared = 0;

    // Daily quota (resets daily, date checked in GetOrLoadStats/
    // OnPlayerCreatureKill - see kill_milestones.cpp). dailyCompletions -
    // how many times the daily quota has already been completed (NEVER
    // resets, source of the permanent XP buff).
    uint32 dailyKills       = 0;
    uint32 dailyCompletions = 0;
};

// Daily contract - a specific creature type in the player's current zone;
// killing it grants a one-time (once per day) XP reward. Shown in the
// NPC's [Daily Contract] menu.
struct KTDailyContract
{
    bool hasContract   = false;
    bool completed      = false;
    uint32 creatureEntry = 0;
    std::string creatureName;
    std::string areaName;  // sub-zone where the target lives (AreaTable.dbc)
    float posX = 0, posY = 0, posZ = 0;
    // ID of the zone the target was found in (player->GetZoneId() at
    // generation time) - needed to convert posX/posY into client map
    // percent coordinates via Map2ZoneCoordinates. Computed LIVE every time
    // it's displayed (KT_GetContractMapCoords) rather than stored as a
    // ready percent, so it never gets stuck with stale/incorrect values if
    // the conversion ever needs fixing.
    uint32 zoneId = 0;
    uint32 rewardXp = 0;
};

// Computes the contract's posX/posY in client map format (0-100%) for the
// TomTom "/way X Y" command - wraps Map2ZoneCoordinates.
void KT_GetContractMapCoords(KTDailyContract const& contract, float& outX, float& outY);

// One zone entry in a player's personal kill statistics (for "favorite
// farming zone").
struct KTZoneKillEntry
{
    uint32 zoneId = 0;
    std::string zoneName;
    uint32 kills = 0;
};

// A creature type the player has killed the most (for the NPC's
// [Top Kills] menu).
struct KTCreatureKillEntry
{
    uint32 creatureEntry = 0;
    std::string name;      // from creature_template.name (sObjectMgr, already in memory)
    uint32 kills = 0;
};

// One row in the leaderboard (the NPC's [Leaderboard] menu).
struct KTLeaderboardEntry
{
    std::string name;
    uint32 totalKills = 0;
};

// Implemented in kill_milestones.cpp: returns (and lazily loads from the DB
// if needed) a player's kill statistics.
KTStats const& KT_GetStats(Player* player);

// Top-N creature types the player has killed most often (from
// mq_kill_stats_by_creature).
std::vector<KTCreatureKillEntry> KT_GetTopCreatures(Player* player, uint32 limit);

// Top-N players on the server by total_kills (across all characters in
// mq_kill_stats).
std::vector<KTLeaderboardEntry> KT_GetLeaderboard(uint32 limit);

// List of bosses (rank = CREATURE_ELITE_WORLDBOSS) the player has killed at
// least once - for the [Boss Bonuses] menu.
std::vector<KTCreatureKillEntry> KT_GetBossKills(Player* player);

// List of ELITE/RARE kills (rank = ELITE, RAREELITE or RARE - not normal,
// and not boss/WORLDBOSS) the player has made - for the [Rare/Elite] menu.
std::vector<KTCreatureKillEntry> KT_GetEliteRareKills(Player* player);

// One boss in the location-grouped [Boss Bonuses] list - unlike
// KTCreatureKillEntry, this carries a continent/location and ready-made
// TomTom pin coordinates (when available).
// ABOUT PINS (/way): TomTom can only reliably place a pin in the player's
// CURRENTLY open zone ("/way X Y description" without a zone name - the
// same format used by the daily contract). The text form
// "/way <Zone> X Y description" (searching all world zones by NAME) proved
// UNRELIABLE on this server/client - repeatedly confirmed with duplicate/
// ambiguous zone names across the client's map data, causing TomTom to
// jump to the first match rather than necessarily the correct one.
// Because of this, the pin only fires if the player is PHYSICALLY standing
// in the same zone/instance as the target (checked in npc_kill_tracker.cpp
// via the player's GetMapId()/GetZoneId() at click time) - 100% reliable,
// with no zone name lookup at all.
struct KTBossEntry
{
    uint32 creatureEntry = 0;
    std::string name;
    std::string continent; // "Eastern Kingdoms" / "Kalimdor" / "Outland" / "Northrend" / "Dungeons"
    std::string location;  // zone name (open world) or dungeon/raid name - display only (enUS)
    std::string level;     // "70" or "70-72" (creature_template.minlevel/maxlevel) - for display
    uint32 minLevel = 0;    // same, but numeric - for programmatic comparisons (weekly contract)
    bool isDungeon = false;

    // "On-site" pin - for a regular (non-dungeon) boss this is its own
    // zone; for a dungeon boss, coordinates INSIDE the instance itself
    // (fires once the player is already there).
    bool hasOwnPin = false;
    uint32 ownMapId = 0;
    uint32 ownZoneId = 0;
    float ownPinX = 0, ownPinY = 0;

    // ENTRANCE pin (dungeons only) - coordinates of the outside point used
    // to enter the instance (fires when the player is standing OUTSIDE, in
    // the same zone as the entrance itself).
    bool hasEntrancePin = false;
    uint32 entranceMapId = 0;
    uint32 entranceZoneId = 0;
    float entrancePinX = 0, entrancePinY = 0;
};

// ALL bosses (creature_template.rank = WORLDBOSS) that are actually spawned
// somewhere in the world, grouped by continent/location (for the
// [Boss Bonuses] menu). For dungeon/raid bosses the continent is always
// "Dungeons", the location is the instance name, and the pin (if resolved)
// points to the OUTSIDE entrance, not a point inside the instance.
// The list has a stable sort order (continent -> location -> name) - the
// indices in this vector are safe to use as menu item IDs between display
// and click handling within a process session (the data doesn't change
// without a restart).
std::vector<KTBossEntry> KT_GetAllBossLocations();

// Set of creature_entry values the player has ALREADY been credited for
// (mq_boss_credit) - used to color-code the KT_GetAllBossLocations list
// green/red.
std::vector<uint32> KT_GetCreditedBossEntries(Player* player);

// Clears the entire in-memory cache (for .killtracker reload) - the next
// access will re-read everything from the DB.
void KT_ResetCache();

// The player's daily contract - generated lazily (on first access each day,
// or via the [Reload] button in the menu, if not yet completed) and cached
// alongside the rest of the stats.
KTDailyContract const& KT_GetOrGenerateContract(Player* player);

// Forcibly regenerate the daily contract (the [Reload Contract] gossip
// button) - in case an unreasonable target was rolled (a friendly/green
// mob, etc). The target is picked again in the player's CURRENT zone (the
// same PickContractTarget used for normal generation). Not available (and
// must NOT be called) if today's contract is already completed - otherwise
// a player could "wash away" an already-earned reward and get a new one.
// Returns false if the contract is already completed or no suitable target
// could be found.
bool KT_RerollContract(Player* player);

// Top-N zones where the player has killed the most enemies (for "favorite
// farming zone").
std::vector<KTZoneKillEntry> KT_GetTopZones(Player* player, uint32 limit);

// One row in the history of COMPLETED daily contracts (mq_contract_history -
// a separate append-only table, NOT mq_daily_contract, which is overwritten
// daily via REPLACE INTO and keeps no history).
struct KTContractHistoryEntry
{
    std::string date;         // contract_date as YYYY-MM-DD (SQL DATE as text)
    std::string creatureName;
    std::string areaName;
    uint32 rewardXp = 0;
};

// ---- Weekly bonus contract ----
// A system separate from the daily one: once per calendar week (the same
// boundary for all characters) - the target is a RANDOM boss (rank=WORLDBOSS)
// from the same list used for [Boss Bonuses] (KT_GetAllBossLocations) - no
// new target-selection logic needed. If the randomly chosen boss is out of
// the player's reach by level (boss minLevel > player level), the EASIEST
// boss (lowest minLevel) from the list is used instead, so low-level
// characters always have at least a theoretically reachable target.
// Reward: twice the XP a daily contract would give at the same level
// (KT_CONTRACT_XP_BASE etc., multiplied by 2) plus a flat
// KT_WEEKLY_GOLD_REWARD gold.
struct KTWeeklyContract
{
    bool hasContract = false;
    bool completed   = false;
    uint32 creatureEntry = 0;
    std::string creatureName;
    std::string continent;
    std::string location;
    bool isDungeon = false;

    // The same two pins as KTBossEntry - copied from there at generation
    // time (bosses don't move, so the coordinates stay valid).
    bool hasOwnPin = false;
    uint32 ownMapId = 0;
    uint32 ownZoneId = 0;
    float ownPinX = 0, ownPinY = 0;

    bool hasEntrancePin = false;
    uint32 entranceMapId = 0;
    uint32 entranceZoneId = 0;
    float entrancePinX = 0, entrancePinY = 0;

    uint32 rewardXp = 0;
};

constexpr uint32 KT_WEEKLY_GOLD_REWARD = 400;

// The player's weekly contract - see the comment on KT_GetOrGenerateContract.
KTWeeklyContract const& KT_GetOrGenerateWeeklyContract(Player* player);

// The player's last N COMPLETED weekly contracts (newest first) - for the
// [Weekly Contract] gossip menu (the same append-only approach as the
// daily KT_GetContractHistory).
std::vector<KTContractHistoryEntry> KT_GetWeeklyContractHistory(Player* player, uint32 limit);

// The player's last N COMPLETED daily contracts (newest first) - for the
// [Daily Contract] menu.
std::vector<KTContractHistoryEntry> KT_GetContractHistory(Player* player, uint32 limit);

// Shared constants for the kill-reward system - same approach as
// quest_milestones.h in mod-missed-quests, split into a header so the NPC
// (Status/progress display) and the actual buff granting logic
// (kill_milestones.cpp) never disagree on the numbers.
constexpr uint32 KT_MILESTONE_STEP     = 400;    // every 400 kills (any type)
constexpr uint32 KT_MILESTONE_MAX_TIER = 100;    // max tier 100 (40000 kills)

// Each tier is an INDEPENDENT pair of spells (A+B, 6 effects total - a
// single spell_dbc row holds at most 3), which is never removed. Tiers
// stack, so the total bonus at tier N = these values * N:
constexpr uint32 KT_ARMOR_PER_TIER        = 100; // +100 armor per tier
constexpr uint32 KT_ATTACK_POWER_PER_TIER = 50;  // +50 attack power (melee/ranged/spell) per tier
constexpr uint32 KT_HASTE_PCT_PER_TIER    = 3;   // +3% attack speed (melee/ranged/cast) per tier

// Tier N -> spellId A = BASE_A + (N-1), spellId B = BASE_B + (N-1)
constexpr uint32 KT_MILESTONE_BASE_SPELL_A = 900500; // Armor + attack power (melee/ranged)
constexpr uint32 KT_MILESTONE_BASE_SPELL_B = 900700; // Spell power + attack speed (melee/ranged/cast)

// ---- Kill streak (no death) ----
// Every KT_STREAK_THRESHOLD consecutive kills without dying grants a
// temporary buff (not permanent, unlike the tiers). Dying resets the
// counter to 0.
constexpr uint32 KT_STREAK_THRESHOLD        = 50;
constexpr uint32 KT_STREAK_BUFF_SPELL       = 900900;
constexpr uint32 KT_STREAK_BUFF_DURATION_MS = 15 * 60 * 1000; // 15 minutes
constexpr uint32 KT_STREAK_BUFF_DAMAGE_PCT  = 15;              // +15% damage from all sources

// ---- Titles (cosmetic, NOT real client-side WoW titles - just text in
// chat/the NPC menu, since a new title string can't be added without
// patching the client's CharTitles.dbc) ----
constexpr uint32 KT_TITLE_TIER_STEP = 10;  // one title per 10 tiers
constexpr uint32 KT_TITLE_COUNT     = KT_MILESTONE_MAX_TIER / KT_TITLE_TIER_STEP; // 10 titles

// Gold reward for each NEW title: titleIndex(1..10) * 100 gold.
// 1,000,000 copper = 100 gold.
constexpr uint32 KT_TITLE_GOLD_STEP = 1000000;

// index 0..9 corresponds to tiers 10,20,...,100
std::string const& KT_GetTitleName(uint32 titleIndex);

// ---- New boss bonus ----
// For EVERY new (never-before-killed) boss - a permanent +5% to all stats,
// as a separate independent spell (same approach as the tier buffs -
// stacks). Capped at 100 unique bosses tracked (=500% to stats) - any
// excess simply has no extra spell (harmless if the server has fewer
// bosses than that).
constexpr uint32 KT_BOSS_STAT_PCT_PER_BOSS   = 5;
constexpr uint32 KT_BOSS_MAX_UNIQUE          = 100;
constexpr uint32 KT_BOSS_BONUS_BASE_SPELL    = 901000; // spellId = BASE + (N-1), N = 1..100

// ---- Cleared dungeon/raid bonus ----
// For EVERY new (not-yet-credited) unique dungeon/raid - a permanent +3% to
// all stats, same approach as the boss bonus above (separate independent
// spell per instance, stacks). "Cleared" = the creditEntry boss from the
// world DB `instance_encounters` table was killed, for a row with
// lastEncounterDungeon != 0 (i.e. the official Blizzard "this is the final
// boss of this dungeon" criterion - the same one driving LFG rewards).
// IMPORTANT: only creditType=0 (a plain "kill this boss" credit) is
// counted - a small number of instances (Ancient of the Culling of
// Stratholme, Trial of the Champion/Crusader, ICC, Ulduar hard modes) use
// a spell-cast/event credit (creditType=1) instead of killing a specific
// boss, so this simple kill hook does NOT fire for them (a deliberate
// limitation, not a bug - supporting creditType=1 would mean hooking
// OnSpellCast, clearly overkill for a private 2-player server).
constexpr uint32 KT_DUNGEON_STAT_PCT_PER_DUNGEON = 3;
constexpr uint32 KT_DUNGEON_MAX_UNIQUE           = 100;
constexpr uint32 KT_DUNGEON_BONUS_BASE_SPELL     = 902000; // spellId = BASE + (N-1), N = 1..100

// One dungeon/raid/WING in the [Cleared Dungeons] list. The credit key is
// creditEntry (entry of the SPECIFIC final boss from instance_encounters,
// creditType=0), NOT mapId - several distinct wings can share the same
// mapId (e.g. Scarlet Monastery: Graveyard/Library/Armory/Cathedral - 4
// DIFFERENT final bosses, all mapId=189; Maraudon: Purple/Orange Crystals/
// Pristine Waters - 3 different bosses, mapId=349), and deduplicating by
// mapId would drop all but one wing from the list. Normal and heroic
// versions of the SAME boss (same creditEntry, different
// lastEncounterDungeon) naturally collapse into ONE entry - deduplicating
// by creditEntry gives exactly that, with no extra logic.
// mapId is only kept to resolve the continent (FindDungeonEntrance) - the
// same mechanism used for the entrance pin in [Boss Bonuses].
struct KTDungeonEntry
{
    uint32 creditEntry = 0; // final boss entry - the credit KEY (mq_dungeon_credit.credit_entry)
    uint32 mapId = 0;       // continent lookup only, NOT unique (several wings share one mapId)
    std::string name;
    std::string continent; // "Eastern Kingdoms" / "Kalimdor" / "Outland" / "Northrend" / "Other"

    // Level range and group type - read directly from LFGDungeons.dbc (the
    // same record already read in KT_GetAllDungeons() for the continent -
    // lfg->MinLevel/MaxLevel/TypeID). MaxLevel in the dbc for raids/endgame
    // content is almost always 83 (just "no upper bound", NOT the real
    // WotLK level cap) - so it must be clamped to KT_LEVEL_CAP_FOR_DISPLAY
    // for display (see the .cpp), rather than shown as-is.
    uint32 minLevel = 0;
    uint32 maxLevel = 0;
    bool isRaid = false; // LFGDungeonEntry::TypeID == 2 (LFG_TYPE_RAID)

    // Raw LFGDungeons.dbc record id (the same value as lastEncounterDungeon
    // in instance_encounters, which KT_GetAllDungeons() already reads) -
    // needed for [Teleport to Dungeon]: instead of a direct TeleportTo
    // (which proved unreliable for a point INSIDE - it broke the instance
    // bind), the button now queues the player via LFGMgr::JoinLfg - the
    // same path used by the standard "Looking For Group" window - and the
    // server itself handles the correct teleport once the proposal forms.
    uint32 lfgDungeonId = 0;
};

// Full list of dungeons/raids/wings (deduplicated by creditEntry - see the
// KTDungeonEntry comment above). Sorted the same way as
// KT_GetAllBossLocations (continent -> name).
std::vector<KTDungeonEntry> KT_GetAllDungeons();

// creditEntry values of all dungeons/wings the player has already been
// credited for - used to color-code the [Cleared Dungeons] list green/red.
std::vector<uint32> KT_GetCreditedDungeonEntries(Player* player);

// Manual toggle of dungeon/wing credit (clicking an entry in the
// [Cleared Dungeons] gossip menu) - for cases where the auto-tracker
// (instance_encounters, creditType=0) doesn't see a particular dungeon's/
// wing's final boss, or if the player deliberately wants to remove an
// already-granted credit. Correctly grants/removes the bonus spells
// (+3%/dungeon) to match the new count. Returns true if credit was JUST
// added, false if it was removed.
// creditEntry - the final boss entry (KTDungeonEntry::creditEntry).
bool KT_ToggleDungeonCredit(Player* player, uint32 creditEntry);

// ---- Daily quota ----
// KT_DAILY_NORM kills within the current game-day (resets at midnight
// server time, checked by date comparison - the "primary" mechanism
// instead of a cron task). For EVERY time the quota is reached - an
// INDEPENDENT permanent +1% XP spell (SPELL_AURA_MOD_XP_PCT), stacking
// (like the tier buffs), up to KT_DAILY_MAX_COMPLETIONS spells (=100% on
// top, no more needed).
constexpr uint32 KT_DAILY_NORM              = 100;
constexpr uint32 KT_DAILY_XP_BUFF_BASE_SPELL = 901200; // spellId = BASE + (N-1), N = 1..100
constexpr uint32 KT_DAILY_MAX_COMPLETIONS    = 100;

// ---- Daily contract ----
// One target (a specific creature type) in the player's CURRENT zone per
// day. Reward - a one-time XP amount computed as
// BASE + (level - BASE_LEVEL) * PER_LEVEL.
constexpr uint32 KT_CONTRACT_XP_BASE       = 10000;
constexpr uint32 KT_CONTRACT_XP_BASE_LEVEL = 35;
constexpr uint32 KT_CONTRACT_XP_PER_LEVEL  = 1000;

// In addition to XP - a flat gold reward for completing the daily contract
// (Player::ModifyMoney counts in copper: 1 gold = 10000 copper).
constexpr uint32 KT_CONTRACT_GOLD_REWARD = 100;

// ---- Mirror of the quest-milestone tier constants (mod-missed-quests/src/
// quest_milestones.h: QM_MILESTONE_STEP/QM_MILESTONE_MAX_TIER) ----
// Deliberately DUPLICATED here rather than #included from the other module -
// AzerothCore modules are built independently of each other (separate
// CMake targets), so a cross-module #include risks breaking the build. The
// source of truth for the actual tier numbers remains mod-missed-quests -
// if QM_MILESTONE_STEP/QM_MILESTONE_MAX_TIER ever change there, these two
// values need to be updated by hand as well (the completed-quest counter
// itself is read directly from Player::GetRewardedQuestCount() - a built-in
// core API, so it doesn't require the mod-missed-quests module at all).
constexpr uint32 KT_QUEST_MILESTONE_STEP     = 20;
constexpr uint32 KT_QUEST_MILESTONE_MAX_TIER = 100;
// mod-missed-quests/quest_milestones.h: QM_MILESTONE_PERCENT - needed here
// only for the aggregated "All Stats" line on the HUD's [Bonuses] tab
// (KT_SendHudUpdate) - the buff itself is still granted exclusively by
// mod-missed-quests.
constexpr uint32 KT_QUEST_MILESTONE_PERCENT  = 5;

// XP percent for ONE daily-quota completion (KT_DAILY_XP_BUFF_BASE_SPELL,
// EffectBasePoints=1 in kt_daily_xp_spells.sql) - extracted into a constant
// only for aggregation on the HUD's [Bonuses] tab (the buff itself is still
// hardcoded in SQL, not used here).
constexpr uint32 KT_DAILY_XP_PCT_PER_COMPLETION = 1;

// ---- Cosmetic trophies ----
// Every KT_TROPHY_TIER_STEP tiers (400 kills/tier) - a cosmetic legendary
// trophy weapon (appearance/particles only, no stats) is added directly to
// the inventory. itemId = KT_TROPHY_BASE_ITEM + (tier / KT_TROPHY_TIER_STEP - 1),
// tier 20->0, 40->1, 60->2, 80->3, 100->4 (5 items total).
constexpr uint32 KT_TROPHY_TIER_STEP = 20;
constexpr uint32 KT_TROPHY_BASE_ITEM = 601080;

// ---- Elite/rare bonus ----
// "Elite/rare" here means anything that is NOT a normal mob (rank !=
// NORMAL) and NOT a boss (rank != WORLDBOSS) - i.e. ELITE(1), RAREELITE(2)
// and RARE(4) combined. Counted from KTStats::elite - THIS SAME counter is
// now shared with the group (like daily/contract/boss credit - see
// ShareGroupKillCredit in kill_milestones.cpp), so "personal kills" in the
// Status menu and the "elite/rare bonus" always stay in sync, no separate
// counter needed. Every KT_ELITE_TIER_STEP such kills grants an
// INDEPENDENT permanent +3% to all stats spell
// (SPELL_AURA_MOD_TOTAL_STAT_PERCENTAGE, the same approach as the boss
// bonus), stacking. Capped at 100 tiers (5000 kills = +300%).
constexpr uint32 KT_ELITE_TIER_STEP        = 50;
constexpr uint32 KT_ELITE_MAX_TIER         = 100;
constexpr uint32 KT_ELITE_STAT_PCT_PER_TIER = 3;
constexpr uint32 KT_ELITE_BONUS_BASE_SPELL  = 901400; // spellId = BASE + (tier-1), tier = 1..100

// A second, PARALLEL bonus on the SAME tiers (KT_ELITE_TIER_STEP/
// KT_ELITE_MAX_TIER, same KTStats::elite counter) - accuracy: +3%/tier to
// melee and ranged combined (SPELL_AURA_MOD_HIT_CHANCE=54 - on this core, a
// single aura covers melee+ranged, there is no separate "ranged hit" aura)
// and +3%/tier separately to spell hit chance
// (SPELL_AURA_MOD_SPELL_HIT_CHANCE=55). Granted/tracked in the same loop as
// KT_ELITE_BONUS_BASE_SPELL - see CheckEliteTierMilestones.
constexpr uint32 KT_ELITE_HIT_PCT_PER_TIER     = 3;
constexpr uint32 KT_ELITE_HIT_BONUS_BASE_SPELL = 901500; // spellId = BASE + (tier-1), tier = 1..100

// ---- Gossip list pagination (bosses/rare-elites) ----
// The client/server hard-caps gossip menus at 32 entries
// (GOSSIP_MAX_MENU_ITEMS in GossipDef.h, an ASSERT fires if exceeded -
// exceeding it CRASHES the server). Boss/elite lists can be longer than 32,
// so they're split into pages, leaving room for [Back]/[Next]/[Previous].
constexpr uint32 KT_LIST_PAGE_SIZE = 28;

// ---- Skill/profession/weapon proficiency bonus ----
// "For every 10 skill points - +1 (NOT %) to all stats." Unlike ALL other
// bonuses above (permanent learnSpell spells granted ONCE and never
// removed) - the source here (character_skill) can change at any moment
// (the player levels a profession), while spell_dbc effects are static
// server-wide (identical for everyone). So this bonus does NOT go through
// spell_dbc/learnSpell - it's computed LIVE (KT_GetSkillBonusPoints) and
// applied directly via
// Unit::HandleStatFlatModifier(UNIT_MOD_STAT_*, TOTAL_VALUE, ...) - the
// same low-level call used internally by the SPELL_AURA_MOD_STAT aura
// handler (AuraEffect::HandleAuraModStat, SpellAuraEffects.cpp), just
// without an actual aura/spell wrapping it. KT_ApplySkillStatBonus computes
// the delta from the previously applied value itself (an in-memory cache in
// kill_milestones.cpp) and applies only the difference - called on login
// and on EVERY skill change (PLAYERHOOK_ON_UPDATE_SKILL/ON_SET_SKILL).
//
// Skill categories counted (SkillLineEntry::categoryId, SharedDefines.h) -
// EXACTLY the ones requested: professions (primary + secondary) and
// weapon/defense skills. Languages/racial traits/class skills (e.g. a
// rogue's flametongue-style abilities) are deliberately excluded.
constexpr uint32 KT_SKILL_POINTS_PER_BONUS = 10; // every 10 skill points = +1

struct KTSkillBonusEntry
{
    uint32 skillId = 0;
    std::string name;
    uint16 value = 0;
    uint16 maxValue = 0;
    uint32 bonusPoints = 0; // value / KT_SKILL_POINTS_PER_BONUS
};

// Raw list of ALL of the player's current skills (counted categories only -
// professions/secondary professions/weapons), for the
// [Skills & Professions] gossip menu.
std::vector<KTSkillBonusEntry> KT_GetSkillBonusEntries(Player* player);

// Sum of bonusPoints across ALL counted skills - this is exactly how many
// +1 points get added to EACH of the 5 stats. Used both by
// KT_ApplySkillStatBonus (applying it) and the HUD/gossip (displaying it).
uint32 KT_GetSkillBonusPoints(Player* player);

// Recomputes and (re)applies the bonus - call on login and on every skill
// change.
void KT_ApplySkillStatBonus(Player* player);

// Clears the internal "how much has already been applied" cache for a
// player who has logged out (the effect itself only lives on a live Unit
// object and disappears with it anyway - this call just avoids keeping
// garbage in the map).
void KT_ClearSkillStatBonusCache(Player* player);
