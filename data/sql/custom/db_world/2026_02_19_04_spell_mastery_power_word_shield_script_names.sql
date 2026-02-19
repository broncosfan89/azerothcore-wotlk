DELETE FROM `spell_script_names`
WHERE `spell_id` = -17
  AND `ScriptName` IN ('spell_pri_power_word_shield_mastery', 'spell_pri_power_word_shield_mastery_aura');

INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`)
VALUES
    (-17, 'spell_pri_power_word_shield_mastery'),
    (-17, 'spell_pri_power_word_shield_mastery_aura');

-- Verification (manual):
-- USE acore_world;
-- SELECT spell_id, ScriptName FROM spell_script_names
-- WHERE spell_id IN (17, -17)
--    OR ScriptName IN ('spell_pri_power_word_shield_mastery', 'spell_pri_power_word_shield_mastery_aura')
-- ORDER BY spell_id, ScriptName;
