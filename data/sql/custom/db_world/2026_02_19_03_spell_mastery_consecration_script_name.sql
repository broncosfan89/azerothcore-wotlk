DELETE FROM `spell_script_names`
WHERE `spell_id` = -26573 AND `ScriptName` = 'spell_pal_consecration_mastery';

INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`)
VALUES
    (-26573, 'spell_pal_consecration_mastery');

-- Verification (manual):
-- USE acore_world;
-- SELECT spell_id, ScriptName FROM spell_script_names
-- WHERE spell_id IN (26573, -26573) OR ScriptName = 'spell_pal_consecration_mastery'
-- ORDER BY spell_id, ScriptName;
