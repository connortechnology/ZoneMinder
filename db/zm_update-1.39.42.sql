--
-- This updates a 1.39.41 database to 1.39.42
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
