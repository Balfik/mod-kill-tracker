-- mod-kill-tracker — characters database schema
-- Single consolidated file (final schema). No server-specific seed data included.

-- Per-character kill statistics. One row per character. Counts ALL kills
-- (player killing blow), split by target rank (normal/elite/boss) via
-- creature_template.rank. Also carries the streak/leaderboard/top-mob-pace
-- counters, unique boss count, daily quota tracking, shared elite/rare bonus
-- credit, and cleared-dungeon count.
CREATE TABLE IF NOT EXISTS `mq_kill_stats` (
    `guid` INT UNSIGNED NOT NULL,
    `total_kills` INT UNSIGNED NOT NULL DEFAULT 0,
    `normal_kills` INT UNSIGNED NOT NULL DEFAULT 0,
    `elite_kills` INT UNSIGNED NOT NULL DEFAULT 0,
    `boss_kills` INT UNSIGNED NOT NULL DEFAULT 0,
    `streak_kills` INT UNSIGNED NOT NULL DEFAULT 0,
    `best_streak` INT UNSIGNED NOT NULL DEFAULT 0,
    `first_kill_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `unique_bosses_killed` INT UNSIGNED NOT NULL DEFAULT 0,
    `daily_kills` INT UNSIGNED NOT NULL DEFAULT 0,
    `daily_date` DATE NULL DEFAULT NULL,
    `daily_completions` INT UNSIGNED NOT NULL DEFAULT 0,
    `elite_bonus_credit` INT UNSIGNED NOT NULL DEFAULT 0,
    `dungeons_cleared` INT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Personal "most-killed creature" stats, used by the [Top Kills] gossip menu.
-- One row per (character, creature) pair.
CREATE TABLE IF NOT EXISTS `mq_kill_stats_by_creature` (
    `guid` INT UNSIGNED NOT NULL,
    `creature_entry` INT UNSIGNED NOT NULL,
    `kills` INT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`guid`, `creature_entry`),
    KEY `idx_guid_kills` (`guid`, `kills`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Personal per-zone kill stats (used for "favorite farming zone" in the
-- Status menu). zone_id/zone_name are read from AreaTable.dbc at kill time.
CREATE TABLE IF NOT EXISTS `mq_kill_stats_by_zone` (
    `guid` INT UNSIGNED NOT NULL,
    `zone_id` INT UNSIGNED NOT NULL,
    `zone_name` VARCHAR(100) NOT NULL DEFAULT '',
    `kills` INT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`guid`, `zone_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Registry of "already received the unique bonus for this specific boss"
-- (kill_milestones.cpp, CheckNewBossKill). Kept independent from
-- mq_kill_stats_by_creature because boss credit is shared with the whole
-- group in radius (ShareGroupKillCredit) — party members who did not land
-- the killing blow still get credit here, without their personal kill
-- counter being incremented.
CREATE TABLE IF NOT EXISTS `mq_boss_credit` (
    `guid` INT UNSIGNED NOT NULL,
    `creature_entry` INT UNSIGNED NOT NULL,
    PRIMARY KEY (`guid`, `creature_entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Registry of "already received the unique bonus for clearing this
-- dungeon/raid wing" (kill_milestones.cpp, CheckNewDungeonClear).
-- credit_entry is the entry of the specific final boss from
-- instance_encounters (NOT the Map.dbc map id) — several distinct wings can
-- share one map id (e.g. Scarlet Monastery's four wings), so keying on the
-- final-boss creditEntry is required for correct per-wing tracking.
CREATE TABLE IF NOT EXISTS `mq_dungeon_credit` (
    `guid` INT UNSIGNED NOT NULL,
    `credit_entry` INT UNSIGNED NOT NULL,
    PRIMARY KEY (`guid`, `credit_entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Current daily contract working row (kill_milestones.cpp:
-- KT_GetOrGenerateContract/CheckContractCompletion). One row per character
-- (PRIMARY KEY guid), overwritten via REPLACE INTO whenever a new contract
-- is generated. zone_id is stored so the target's map-percent coordinates
-- for the TomTom "/way" pin can be computed live every time the contract is
-- displayed (always fresh, never stale).
CREATE TABLE IF NOT EXISTS `mq_daily_contract` (
    `guid` INT UNSIGNED NOT NULL,
    `contract_date` DATE NOT NULL,
    `creature_entry` INT UNSIGNED NOT NULL DEFAULT 0,
    `creature_name` VARCHAR(100) NOT NULL DEFAULT '',
    `area_name` VARCHAR(100) NOT NULL DEFAULT '',
    `pos_x` FLOAT NOT NULL DEFAULT 0,
    `pos_y` FLOAT NOT NULL DEFAULT 0,
    `pos_z` FLOAT NOT NULL DEFAULT 0,
    `zone_id` INT UNSIGNED NOT NULL DEFAULT 0,
    `reward_xp` INT UNSIGNED NOT NULL DEFAULT 0,
    `completed` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Append-only history of COMPLETED daily contracts (kill_milestones.cpp:
-- CheckContractCompletion/KT_GetContractHistory). Unlike mq_daily_contract
-- (one working row per player, overwritten daily), this table gets a new
-- row every time a contract is completed and never deletes/overwrites.
CREATE TABLE IF NOT EXISTS `mq_contract_history` (
    `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
    `guid` INT UNSIGNED NOT NULL,
    `contract_date` DATE NOT NULL,
    `creature_name` VARCHAR(100) NOT NULL DEFAULT '',
    `area_name` VARCHAR(100) NOT NULL DEFAULT '',
    `reward_xp` INT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`id`),
    KEY `idx_guid` (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Current weekly bonus-contract working row (kill_milestones.cpp:
-- KT_GetOrGenerateWeeklyContract). Same principle as mq_daily_contract: one
-- row per player (PRIMARY KEY guid), REPLACE INTO on a new week, no history
-- kept here (see mq_weekly_contract_history for that).
CREATE TABLE IF NOT EXISTS `mq_weekly_contract` (
    `guid` INT UNSIGNED NOT NULL,
    `week_key` INT UNSIGNED NOT NULL,
    `creature_entry` INT UNSIGNED NOT NULL,
    `creature_name` VARCHAR(100) NOT NULL DEFAULT '',
    `continent` VARCHAR(100) NOT NULL DEFAULT '',
    `location` VARCHAR(100) NOT NULL DEFAULT '',
    `is_dungeon` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `has_own_pin` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `own_map_id` INT UNSIGNED NOT NULL DEFAULT 0,
    `own_zone_id` INT UNSIGNED NOT NULL DEFAULT 0,
    `own_pin_x` FLOAT NOT NULL DEFAULT 0,
    `own_pin_y` FLOAT NOT NULL DEFAULT 0,
    `has_entrance_pin` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `entrance_map_id` INT UNSIGNED NOT NULL DEFAULT 0,
    `entrance_zone_id` INT UNSIGNED NOT NULL DEFAULT 0,
    `entrance_pin_x` FLOAT NOT NULL DEFAULT 0,
    `entrance_pin_y` FLOAT NOT NULL DEFAULT 0,
    `reward_xp` INT UNSIGNED NOT NULL DEFAULT 0,
    `completed` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Append-only history of COMPLETED weekly bonus contracts
-- (kill_milestones.cpp: CheckWeeklyContractCompletion/KT_GetWeeklyContractHistory).
-- Same principle as mq_contract_history for the daily contract.
CREATE TABLE IF NOT EXISTS `mq_weekly_contract_history` (
    `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
    `guid` INT UNSIGNED NOT NULL,
    `completed_date` DATE NOT NULL,
    `creature_name` VARCHAR(100) NOT NULL DEFAULT '',
    `location` VARCHAR(100) NOT NULL DEFAULT '',
    `reward_xp` INT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`id`),
    KEY `idx_guid` (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
