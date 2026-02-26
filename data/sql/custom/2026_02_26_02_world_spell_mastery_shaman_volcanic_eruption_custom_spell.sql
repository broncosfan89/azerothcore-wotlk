DELETE FROM spell_script_names
WHERE spell_id = 770001
  AND ScriptName = 'spell_voa_flaming_cinder';

INSERT INTO spell_script_names (spell_id, ScriptName) VALUES
(770001, 'spell_voa_flaming_cinder');
