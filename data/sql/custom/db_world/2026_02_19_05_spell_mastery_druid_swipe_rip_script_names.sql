DELETE FROM `spell_script_names`
WHERE (`spell_id` = -62078 AND `ScriptName` = 'spell_dru_swipe_cat_mastery')
   OR (`spell_id` = -1079 AND `ScriptName` = 'spell_dru_rip_mastery');

INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`)
VALUES
    (-62078, 'spell_dru_swipe_cat_mastery'),
    (-1079, 'spell_dru_rip_mastery');

-- Verification (manual):
-- USE acore_world;
-- SELECT spell_id, ScriptName FROM spell_script_names
-- WHERE spell_id IN (62078, -62078, 1079, -1079)
--    OR ScriptName IN ('spell_dru_swipe_cat_mastery', 'spell_dru_rip_mastery')
-- ORDER BY spell_id, ScriptName;
