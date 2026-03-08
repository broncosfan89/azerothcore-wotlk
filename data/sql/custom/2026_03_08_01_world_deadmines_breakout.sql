DELETE FROM `quest_template_addon` WHERE `ID` IN (900000, 900001);
DELETE FROM `quest_template` WHERE `ID` IN (900000, 900001);
DELETE FROM `quest_poi_points` WHERE `QuestID` IN (900000, 900001);
DELETE FROM `quest_poi` WHERE `QuestID` IN (900000, 900001);

INSERT INTO `quest_template`
(`ID`, `QuestType`, `QuestLevel`, `MinLevel`, `QuestSortID`, `RewardXPDifficulty`, `RewardMoney`, `Flags`, `LogTitle`, `LogDescription`, `QuestDescription`, `AreaDescription`, `QuestCompletionLog`, `VerifiedBuild`)
VALUES
(900000, 2, 20, 1, 40, 4, 10000, 65536, 'Break the Defias Uprising', 'Defend Westfall from the Deadmines breakout. Breakout mobs are marked in-world and clustered around the active hotspots.', 'The Defias have spilled out of the Deadmines and are raiding Westfall. Push them back, crush their leaders, and restore order before the countryside collapses into chaos.', 'Repel the Defias breakout in Westfall. Search Moonbrook, the Jangolode road, the southern fields, and the Dagger Hills.', 'Westfall is safe again. Claim your reward from the quest tracker.', 12340),
(900001, 2, 22, 1, 0, 5, 25000, 65536, 'Seal the Deadmines', 'Enter the Deadmines breach and defeat Edwin VanCleef. The quest marker points to the Moonbrook entrance.', 'With the breakout contained, the Defias leadership has retreated into the Deadmines. Enter the breach, survive the hardened resistance, and bring down Edwin VanCleef before the Brotherhood can regroup.', 'Defeat Edwin VanCleef inside the Deadmines breach.', 'The Deadmines breach has been sealed. Claim your reward from the quest tracker.', 12340);

INSERT INTO `quest_template_addon`
(`ID`, `MaxLevel`, `AllowableClasses`, `SourceSpellID`, `PrevQuestID`, `NextQuestID`, `ExclusiveGroup`, `RewardMailTemplateID`, `RewardMailDelay`, `RequiredSkillID`, `RequiredSkillPoints`, `RequiredMinRepFaction`, `RequiredMaxRepFaction`, `RequiredMinRepValue`, `RequiredMaxRepValue`, `ProvidedItemCount`, `SpecialFlags`)
VALUES
(900000, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 6),
(900001, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 6);

INSERT INTO `quest_poi`
(`QuestID`, `id`, `ObjectiveIndex`, `MapID`, `WorldMapAreaId`, `Floor`, `Priority`, `Flags`, `VerifiedBuild`)
VALUES
(900000, 0, -1, 0, 39, 0, 0, 1, 12340),
(900000, 1, -1, 0, 39, 0, 0, 1, 12340),
(900000, 2, -1, 0, 39, 0, 0, 1, 12340),
(900000, 3, -1, 0, 39, 0, 0, 1, 12340),
(900000, 4, -1, 0, 39, 0, 0, 1, 12340),
(900001, 0, -1, 0, 39, 0, 0, 1, 12340);

INSERT INTO `quest_poi_points`
(`QuestID`, `Idx1`, `Idx2`, `X`, `Y`, `VerifiedBuild`)
VALUES
(900000, 0, 0, -9940, 1430, 12340),
(900000, 1, 0, -10335, 1490, 12340),
(900000, 2, 0, -11015, 1515, 12340),
(900000, 3, 0, -10315, 1915, 12340),
(900000, 4, 0, -11225, 1260, 12340),
(900001, 0, 0, -11284, 1450, 12340);
