-- Mythic Utgarde Keep boss-corpse loot entries (loot mode 2)
-- Uses cloned Mythic 0 items (980001..980009) from prior migration.
-- Runtime code sets bosses to LOOT_MODE_HARD_MODE_1 in Mythic UK,
-- while non-boss mobs are set to loot mode 0 (no drops).

DELETE FROM creature_loot_template
WHERE Entry IN (23953, 23954, 24200, 24201)
  AND LootMode = 2
  AND Item BETWEEN 980001 AND 980009;

INSERT INTO creature_loot_template
    (Entry, Item, Reference, Chance, QuestRequired, LootMode, GroupId, MinCount, MaxCount, Comment)
VALUES
    -- Prince Keleseth (pick 1 from 3)
    (23953, 980001, 0, 0, 0, 2, 1, 1, 1, 'Prince Keleseth - Mythic 0 upgraded loot'),
    (23953, 980002, 0, 0, 0, 2, 1, 1, 1, 'Prince Keleseth - Mythic 0 upgraded loot'),
    (23953, 980003, 0, 0, 0, 2, 1, 1, 1, 'Prince Keleseth - Mythic 0 upgraded loot'),

    -- Skarvald/Dalronn (same encounter pool, pick 1 from 3)
    (24200, 980004, 0, 0, 0, 2, 1, 1, 1, 'Skarvald - Mythic 0 upgraded loot'),
    (24200, 980005, 0, 0, 0, 2, 1, 1, 1, 'Skarvald - Mythic 0 upgraded loot'),
    (24200, 980006, 0, 0, 0, 2, 1, 1, 1, 'Skarvald - Mythic 0 upgraded loot'),
    (24201, 980004, 0, 0, 0, 2, 1, 1, 1, 'Dalronn - Mythic 0 upgraded loot'),
    (24201, 980005, 0, 0, 0, 2, 1, 1, 1, 'Dalronn - Mythic 0 upgraded loot'),
    (24201, 980006, 0, 0, 0, 2, 1, 1, 1, 'Dalronn - Mythic 0 upgraded loot'),

    -- Ingvar the Plunderer (pick 1 from 3)
    (23954, 980007, 0, 0, 0, 2, 1, 1, 1, 'Ingvar - Mythic 0 upgraded loot'),
    (23954, 980008, 0, 0, 0, 2, 1, 1, 1, 'Ingvar - Mythic 0 upgraded loot'),
    (23954, 980009, 0, 0, 0, 2, 1, 1, 1, 'Ingvar - Mythic 0 upgraded loot');

-- Verification query (run manually if needed):
-- SELECT Entry, Item, LootMode, GroupId, Chance
-- FROM creature_loot_template
-- WHERE Entry IN (23953,23954,24200,24201) AND LootMode = 2
-- ORDER BY Entry, GroupId, Item;
