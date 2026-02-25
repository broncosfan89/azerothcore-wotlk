-- Rebase existing character-held Mythic UK items to low safe IDs
UPDATE item_instance
SET itemEntry = CASE itemEntry
    WHEN 980001 THEN 59001
    WHEN 980002 THEN 59002
    WHEN 980003 THEN 59003
    WHEN 980004 THEN 59004
    WHEN 980005 THEN 59005
    WHEN 980006 THEN 59006
    WHEN 980007 THEN 59007
    WHEN 980008 THEN 59008
    WHEN 980009 THEN 59009
    ELSE itemEntry
END
WHERE itemEntry BETWEEN 980001 AND 980009;

-- Verification:
-- SELECT itemEntry, COUNT(*) AS cnt FROM item_instance WHERE itemEntry BETWEEN 59001 AND 59009 GROUP BY itemEntry ORDER BY itemEntry;
