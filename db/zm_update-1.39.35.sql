--
-- Add DeviceFrameBudget to Monitors: a per-monitor cap on how many decoded
-- hardware frames the capture daemon will pin on the accelerator card, NULL
-- meaning "use ZM_DEVICE_FRAME_BUDGET".
--
-- This was originally added to zm_update-1.39.26.sql.in. That migration had
-- already shipped, so every install upgraded past 1.39.26 never re-ran it and
-- never got the column, while fresh installs got it from zm_create.sql.in --
-- which is exactly the split that makes the fault invisible when testing from
-- a clean database. Reissued here at the next free number.
--

SET @s = (SELECT IF(
    (SELECT COUNT(*) FROM INFORMATION_SCHEMA.COLUMNS WHERE table_schema = DATABASE()
     AND table_name = 'Monitors' AND column_name = 'DeviceFrameBudget'
    ) > 0,
"SELECT 'Column DeviceFrameBudget already exists'",
"ALTER TABLE `Monitors` ADD `DeviceFrameBudget` int(10) default NULL AFTER `DecoderHWAccelDevice`"
));

PREPARE stmt FROM @s;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;
