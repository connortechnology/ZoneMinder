--
-- Add AnalysisImageOpacity setting to Monitors table
--

SET @s = (SELECT IF(
    (SELECT COUNT(*) FROM INFORMATION_SCHEMA.COLUMNS WHERE table_schema = DATABASE()
     AND table_name = 'Monitors'
     AND column_name = 'AnalysisImageOpacity'
    ) > 0,
"SELECT 'Column AnalysisImageOpacity already exists in Monitors'",
"ALTER TABLE `Monitors` ADD COLUMN `AnalysisImageOpacity` TINYINT UNSIGNED NOT NULL DEFAULT '128' AFTER `AnalysisImage`"
));

PREPARE stmt FROM @s;
EXECUTE stmt;
