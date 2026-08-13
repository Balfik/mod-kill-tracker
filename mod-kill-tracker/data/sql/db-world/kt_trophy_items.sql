-- 5 cosmetic "hunter trophies" (kill_milestones.cpp, GrantTrophyIfReached),
-- awarded at tiers 20/40/60/80/100 (every 20 tiers = 8000 kills). NO
-- stats/damage (dmg/stat_* left at default zero) — cosmetic only. displayid
-- values are borrowed from real legendary weapons so they look impressive:
--   601080 - looks like Thunderfury (one-handed sword)
--   601081 - looks like Sulfuras, Hand of Ragnaros (two-handed mace)
--   601082 - looks like Atiesh (staff)
--   601083 - looks like Val'anyr (one-handed mace)
--   601084 - looks like Shadowmourne (two-handed axe)
-- bonding=1 (BoP), Quality=5 (legendary/orange text), maxcount=1 (unique).
DELETE FROM `item_template` WHERE `entry` BETWEEN 601080 AND 601084;
INSERT INTO `item_template`
    (`entry`, `class`, `subclass`, `name`, `displayid`, `Quality`, `Flags`, `InventoryType`,
     `AllowableClass`, `AllowableRace`, `ItemLevel`, `RequiredLevel`, `maxcount`, `stackable`,
     `ContainerSlots`, `bonding`, `description`, `MaxDurability`, `sheath`, `Material`)
VALUES
    (601080, 2, 7,  'Trophy: Echo of Thunderfury',  30606, 5, 0, 13, -1, -1, 1, 1, 1, 1, 0, 1,
     'A cosmetic hunter trophy - an exact visual copy of the legendary blade, no combat stats.', 100, 1, 2),
    (601081, 2, 5,  'Trophy: Echo of the Hand of Ragnaros', 29698, 5, 0, 17, -1, -1, 1, 1, 1, 1, 0, 1,
     'A cosmetic hunter trophy - an exact visual copy of the legendary mace, no combat stats.', 100, 1, 4),
    (601082, 2, 10, 'Trophy: Echo of Atiesh',  35632, 5, 0, 17, -1, -1, 1, 1, 1, 1, 0, 1,
     'A cosmetic hunter trophy - an exact visual copy of the legendary staff, no combat stats.', 100, 1, 7),
    (601083, 2, 4,  'Trophy: Echo of Val'anyr', 61655, 5, 0, 13, -1, -1, 1, 1, 1, 1, 0, 1,
     'A cosmetic hunter trophy - an exact visual copy of the legendary mace, no combat stats.', 100, 1, 4),
    (601084, 2, 1,  'Trophy: Echo of Shadowmourne', 65153, 5, 0, 17, -1, -1, 1, 1, 1, 1, 0, 1,
     'A cosmetic hunter trophy - an exact visual copy of the legendary axe, no combat stats.', 100, 1, 2);
