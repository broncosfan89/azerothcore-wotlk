-- Mythic Utgarde Keep M0 custom item clones (higher ilvl/stats)
-- Source heroic entries:
--   35570,35571,35572 (Keleseth)
--   35573,35574,35575 (Skarvald & Dalronn)
--   35576,35577,35578 (Ingvar)
-- Clone entries:
--   980001..980009 (same order as above)

DELETE FROM item_template WHERE entry BETWEEN 980001 AND 980009;

DROP TEMPORARY TABLE IF EXISTS tmp_mythic_uk_items;
CREATE TEMPORARY TABLE tmp_mythic_uk_items AS
SELECT *
FROM item_template
WHERE entry IN (35570,35571,35572,35573,35574,35575,35576,35577,35578);

UPDATE tmp_mythic_uk_items
SET entry = CASE entry
    WHEN 35570 THEN 980001
    WHEN 35571 THEN 980002
    WHEN 35572 THEN 980003
    WHEN 35573 THEN 980004
    WHEN 35574 THEN 980005
    WHEN 35575 THEN 980006
    WHEN 35576 THEN 980007
    WHEN 35577 THEN 980008
    WHEN 35578 THEN 980009
    ELSE entry
END;

UPDATE tmp_mythic_uk_items
SET
    name = CONCAT(name, ' (Mythic 0)'),
    Quality = GREATEST(Quality, 4),
    itemlevel = itemlevel + 32,
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
SELECT * FROM tmp_mythic_uk_items;

DROP TEMPORARY TABLE IF EXISTS tmp_mythic_uk_items;

-- Verification query (run manually if needed):
-- SELECT entry, name, itemlevel, Quality, stat_value1, stat_value2, stat_value3, dmg_min1, dmg_max1, armor
-- FROM item_template
-- WHERE entry BETWEEN 980001 AND 980009
-- ORDER BY entry;
