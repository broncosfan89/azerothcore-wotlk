-- Rebuild Mythic Utgarde Keep custom items (59001..59009) as clean full template clones.
-- This keeps them behaving like standard gear templates while preserving Mythic tuning.

-- 1) Rebuild item_template rows from original UK heroic items.
DELETE FROM item_template WHERE entry BETWEEN 59001 AND 59009;

DROP TEMPORARY TABLE IF EXISTS tmp_mythic_uk_590xx;
CREATE TEMPORARY TABLE tmp_mythic_uk_590xx LIKE item_template;

INSERT INTO tmp_mythic_uk_590xx
SELECT *
FROM item_template
WHERE entry IN (35570,35571,35572,35573,35574,35575,35576,35577,35578);

UPDATE tmp_mythic_uk_590xx
SET entry = CASE entry
    WHEN 35570 THEN 59001
    WHEN 35571 THEN 59002
    WHEN 35572 THEN 59003
    WHEN 35573 THEN 59004
    WHEN 35574 THEN 59005
    WHEN 35575 THEN 59006
    WHEN 35576 THEN 59007
    WHEN 35577 THEN 59008
    WHEN 35578 THEN 59009
    ELSE entry
END;

UPDATE tmp_mythic_uk_590xx
SET
    name = CONCAT(name, ' (Mythic 0)'),
    Quality = GREATEST(Quality, 4),
    ItemLevel = 205,
    bonding = 2,
    stat_value1 = ROUND(stat_value1 * 1.35),
    stat_value2 = ROUND(stat_value2 * 1.35),
    stat_value3 = ROUND(stat_value3 * 1.35),
    stat_value4 = ROUND(stat_value4 * 1.35),
    stat_value5 = ROUND(stat_value5 * 1.35),
    stat_value6 = ROUND(stat_value6 * 1.35),
    stat_value7 = ROUND(stat_value7 * 1.35),
    stat_value8 = ROUND(stat_value8 * 1.35),
    stat_value9 = ROUND(stat_value9 * 1.35),
    stat_value10 = ROUND(stat_value10 * 1.35),
    dmg_min1 = ROUND(dmg_min1 * 1.30, 1),
    dmg_max1 = ROUND(dmg_max1 * 1.30, 1),
    dmg_min2 = ROUND(dmg_min2 * 1.30, 1),
    dmg_max2 = ROUND(dmg_max2 * 1.30, 1),
    armor = ROUND(armor * 1.35),
    block = ROUND(block * 1.35);

INSERT INTO item_template
SELECT *
FROM tmp_mythic_uk_590xx
ORDER BY entry;

DROP TEMPORARY TABLE IF EXISTS tmp_mythic_uk_590xx;

-- 2) Ensure Mythic loot for UK bosses points to 590xx entries.
UPDATE creature_loot_template
SET Item = CASE Item
    WHEN 980001 THEN 59001
    WHEN 980002 THEN 59002
    WHEN 980003 THEN 59003
    WHEN 980004 THEN 59004
    WHEN 980005 THEN 59005
    WHEN 980006 THEN 59006
    WHEN 980007 THEN 59007
    WHEN 980008 THEN 59008
    WHEN 980009 THEN 59009
    ELSE Item
END
WHERE Entry IN (23953, 23954, 24200, 24201)
  AND LootMode = 2
  AND Item IN (980001,980002,980003,980004,980005,980006,980007,980008,980009,59001,59002,59003,59004,59005,59006,59007,59008,59009);

-- 3) Cleanup legacy 980xxx templates (no longer used).
DELETE FROM item_template WHERE entry BETWEEN 980001 AND 980009;

-- Verification:
-- SELECT entry,name,class,subclass,InventoryType,displayid,Quality,ItemLevel,bonding
-- FROM item_template
-- WHERE entry BETWEEN 59001 AND 59009
-- ORDER BY entry;
-- SELECT Entry,Item,LootMode,GroupId,Chance
-- FROM creature_loot_template
-- WHERE Entry IN (23953,23954,24200,24201) AND LootMode=2
-- ORDER BY Entry,Item;
