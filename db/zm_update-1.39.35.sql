--
));

PREPARE stmt FROM @s;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;
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

-- This updates a 1.39.34 database to 1.39.35
--
-- Add Frames.AudioLevel so the event view can graph how loud a monitor was
-- alongside how much it moved.
--
-- zm_update-1.39.31.sql gave Monitors the audio detection settings, but the
-- level itself only ever existed in shared memory, which means it is gone the
-- moment the frame passes. Score has always been persisted per frame; this is
-- the audio equivalent, on the same 0-100 dBFS-derived scale the threshold
-- uses, so the two can share an axis.
--
-- The stored value is the peak since the previous Frames row rather than the
-- level at the instant the row was written. Rows are written well below the
-- capture rate -- only alarm, bulk and score-increasing frames get one -- so
-- sampling at write time would drop exactly the short loud noises that matter
-- on a surveillance timeline.
--
-- The level is measured whenever the monitor has decodable audio, not only
-- when AudioDetection is on. A threshold cannot be chosen without first seeing
-- what the device's floor and peaks are, so the setting governs only whether
-- crossing the threshold scores, never whether the level is recorded.
--
-- 0 therefore means silence, no audio stream, or a codec with no decoder. It
-- is also what every pre-existing row gets. The graph treats an all-zero
-- series as "no audio data" and draws no line, so backfilled rows do not
-- appear as a flat line along the bottom claiming silence was measured.
--

SELECT 'Checking for AudioLevel in Frames';
SET @s = (SELECT IF(
  (SELECT COUNT(*)
  FROM INFORMATION_SCHEMA.COLUMNS
  WHERE table_name = 'Frames'
  AND table_schema = DATABASE()
  AND column_name = 'AudioLevel'
  ) > 0,
"SELECT 'Column AudioLevel already exists in Frames'",
"ALTER TABLE `Frames` ADD COLUMN `AudioLevel` tinyint(3) unsigned NOT NULL default '0' AFTER `Score`"
