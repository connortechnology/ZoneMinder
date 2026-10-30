--
-- This updates a 1.39.44 database to 1.39.45
--
-- Add MotionFrameWidth to Monitors: the width of the frame motion detection
-- runs on, with the height following the capture's aspect ratio.
--
-- Motion detection does not need full resolution. On a 3840x2160 camera the
-- zone check took a median of 139ms a frame even with almost nothing moving,
-- because the cost is the pass over every pixel; at 1280 it checks a ninth of
-- them. Zones are stored against the capture and are moved onto the smaller
-- frame when they are loaded, so nothing already drawn needs changing.
--
-- 0 means the capture resolution, which is how every existing monitor
-- carries on.
--

SELECT 'Checking for MotionFrameWidth in Monitors';
SET @s = (SELECT IF(
  (SELECT COUNT(*)
  FROM INFORMATION_SCHEMA.COLUMNS
  WHERE table_name = 'Monitors'
  AND table_schema = DATABASE()
  AND column_name = 'MotionFrameWidth'
  ) > 0,
"SELECT 'Column MotionFrameWidth already exists in Monitors'",
"ALTER TABLE `Monitors` ADD COLUMN `MotionFrameWidth` smallint(5) unsigned NOT NULL default '0' AFTER `MotionFrameSkip`"
));

PREPARE stmt FROM @s;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

-- An ai_server database that took this change when it was numbered 1.39.42
-- skipped upstream's 1.39.42, which adds scheduled monitor actions. Apply that
-- here too; both checks are no-ops on a database that already has them.

SELECT 'Checking for Schedule in MonitorActions';
SET @s = (SELECT IF(
  (SELECT COUNT(*)
  FROM INFORMATION_SCHEMA.COLUMNS
  WHERE table_name = 'MonitorActions'
  AND table_schema = DATABASE()
  AND column_name = 'Schedule'
  ) > 0,
"SELECT 'Column Schedule already exists in MonitorActions'",
"ALTER TABLE `MonitorActions` ADD COLUMN `Schedule` varchar(64) NOT NULL default '' AFTER `TriggerOn`"
));

PREPARE stmt FROM @s;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SELECT 'Checking for Schedule in the MonitorActions.TriggerOn enum';
SET @s = (SELECT IF(
  (SELECT LOCATE('Schedule', COLUMN_TYPE)
  FROM INFORMATION_SCHEMA.COLUMNS
  WHERE table_name = 'MonitorActions'
  AND table_schema = DATABASE()
  AND column_name = 'TriggerOn'
  ) > 0,
"SELECT 'TriggerOn already offers Schedule'",
"ALTER TABLE `MonitorActions` MODIFY `TriggerOn` enum('EventStart','EventEnd','Alarm','AlarmEnd','Manual','Schedule') NOT NULL default 'EventStart'"
));

PREPARE stmt FROM @s;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;
