-- Temporary "Battle Fervor" buff for a kill streak without dying. NOT
-- passive (unlike the tier buffs) — the player should SEE the icon and
-- timer in-game. Duration (15 min) is set manually in code
-- (kill_milestones.cpp, KT_GrantStreakBuff) via Aura::SetDuration/
-- SetMaxDuration right after CastSpell — DurationIndex is unused here
-- (left at 0), since adding a dedicated spell_duration.dbc row for a
-- single value isn't worth it.
--
-- Effect: SPELL_AURA_MOD_DAMAGE_PERCENT_DONE (79), EffectMiscValue = 127
-- (school mask "all schools": bit 0 = physical, bits 1-6 = magic) = +15%
-- damage from ALL sources (melee/ranged/spell), in one effect.
DELETE FROM `spell_dbc` WHERE `ID` = 900900;
INSERT INTO `spell_dbc`
    (`ID`, `Attributes`, `EquippedItemClass`, `EquippedItemSubclass`, `EquippedItemInvTypes`,
     `Effect_1`, `EffectBasePoints_1`, `EffectAura_1`, `EffectMiscValue_1`, `ImplicitTargetA_1`,
     `SpellIconID`, `Name_Lang_enUS`, `Name_Lang_ruRU`, `Name_Lang_Mask`)
VALUES
    (900900, 0, -1, -1, -1, 6, 15, 79, 127, 1, 1, 'Battle Fervor', 'Boiovyi Zapal', 0x00000002);
