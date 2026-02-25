-- Make Mythic UK custom items tradable before equip (Bind on Equip)
UPDATE item_template
SET bonding = 2
WHERE entry BETWEEN 980001 AND 980009;

-- Verification query:
-- SELECT entry, name, bonding, ItemLevel, displayid, InventoryType
-- FROM item_template
-- WHERE entry BETWEEN 980001 AND 980009
-- ORDER BY entry;
