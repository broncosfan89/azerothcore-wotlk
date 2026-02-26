DELETE FROM spell_script_names
WHERE spell_id = 770001
  AND ScriptName IN ('spell_voa_flaming_cinder', 'spell_sha_volcanic_eruption_cast');

INSERT INTO spell_script_names (spell_id, ScriptName) VALUES
(770001, 'spell_sha_volcanic_eruption_cast');
