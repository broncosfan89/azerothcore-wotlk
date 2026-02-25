-- Raise Mythic Utgarde Keep M0 custom loot to ilvl 205 and tune stats proportionally
UPDATE item_template
SET
    ItemLevel = 205,
    stat_value1 = ROUND(stat_value1 * 1.10),
    stat_value2 = ROUND(stat_value2 * 1.10),
    stat_value3 = ROUND(stat_value3 * 1.10),
    stat_value4 = ROUND(stat_value4 * 1.10),
    stat_value5 = ROUND(stat_value5 * 1.10),
    stat_value6 = ROUND(stat_value6 * 1.10),
    stat_value7 = ROUND(stat_value7 * 1.10),
    stat_value8 = ROUND(stat_value8 * 1.10),
    stat_value9 = ROUND(stat_value9 * 1.10),
    stat_value10 = ROUND(stat_value10 * 1.10),
    dmg_min1 = ROUND(dmg_min1 * 1.10, 1),
    dmg_max1 = ROUND(dmg_max1 * 1.10, 1),
    dmg_min2 = ROUND(dmg_min2 * 1.10, 1),
    dmg_max2 = ROUND(dmg_max2 * 1.10, 1),
    armor = ROUND(armor * 1.10),
    block = ROUND(block * 1.10)
WHERE entry BETWEEN 980001 AND 980009;

-- Verification query:
-- SELECT entry, name, ItemLevel, displayid, InventoryType, stat_value1, stat_value2, dmg_min1, dmg_max1, armor
-- FROM item_template
-- WHERE entry BETWEEN 980001 AND 980009
-- ORDER BY entry;
