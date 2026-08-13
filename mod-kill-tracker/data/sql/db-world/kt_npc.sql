-- Custom NPC "Hunt Chronicler" (npc_kill_tracker)
-- Entry chosen in the same custom range (601xxx) used by this project's
-- other custom modules; adjust if it collides with your own server.
--
-- NOTE: unlike some other custom NPCs, this file deliberately does NOT
-- INSERT into `creature` (no spawn) — the NPC is meant to be placed by hand
-- with a GM command while standing at the desired location:
--     .npc add 601070
DELETE FROM `creature_template` WHERE `entry` = 601070;
INSERT INTO `creature_template`
    (`entry`, `difficulty_entry_1`, `difficulty_entry_2`, `difficulty_entry_3`, `KillCredit1`, `KillCredit2`,
     `name`, `subname`, `IconName`, `gossip_menu_id`, `minlevel`, `maxlevel`, `exp`, `faction`, `npcflag`,
     `speed_walk`, `speed_run`, `speed_swim`, `speed_flight`, `detection_range`, `rank`, `dmgschool`,
     `DamageModifier`, `BaseAttackTime`, `RangeAttackTime`, `BaseVariance`, `RangeVariance`, `unit_class`,
     `unit_flags`, `unit_flags2`, `dynamicflags`, `family`, `type`, `type_flags`, `lootid`, `pickpocketloot`,
     `skinloot`, `PetSpellDataId`, `VehicleId`, `mingold`, `maxgold`, `AIName`, `MovementType`, `HoverHeight`,
     `HealthModifier`, `ManaModifier`, `ArmorModifier`, `ExperienceModifier`, `RacialLeader`, `movementId`,
     `RegenHealth`, `CreatureImmunitiesId`, `flags_extra`, `ScriptName`, `VerifiedBuild`)
VALUES
    (601070, 0, 0, 0, 0, 0,
     'Hunt Chronicler', 'Kill Tracker', NULL, 0, 80, 80, 0, 35, 1,
     1, 1.14286, 1, 1, 20, 0, 0,
     1, 2000, 0, 1, 1, 1,
     0, 0, 0, 0, 7, 138412032, 0, 0,
     0, 0, 0, 0, 0, '', 0, 1,
     1, 1, 1, 1, 0, 0,
     1, 0, 2, 'npc_kill_tracker', NULL);

-- Model (appearance) — CreatureDisplayID 25301 (a Hemet Nesingwary-style
-- vanilla hunter look), thematically fitting for a kill tracker.
DELETE FROM `creature_template_model` WHERE `CreatureID` = 601070;
INSERT INTO `creature_template_model` (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`)
VALUES (601070, 0, 25301, 1, 1, NULL);
