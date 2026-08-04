--
-- Add a composite index on Server_Stats to speed up the per-server latest-stats
-- lookup (SELECT * FROM Server_Stats WHERE ServerId=? ORDER BY TimeStamp DESC LIMIT 1),
-- which was previously a full table scan.
--

SET @s = (SELECT IF(
  (SELECT COUNT(*)
    FROM INFORMATION_SCHEMA.STATISTICS
    WHERE table_name = 'Server_Stats'
    AND table_schema = DATABASE()
    AND index_name = 'Server_Stats_ServerId_idx'
  ) > 0,
  "SELECT 'Server_Stats_ServerId_idx already exists on Server_Stats table'",
  "ALTER TABLE `Server_Stats` ADD INDEX `Server_Stats_ServerId_idx` (`ServerId`, `TimeStamp`)"
));
PREPARE stmt FROM @s;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

--
-- Licence plate recognition, run on the Quadra VPU chained after object
-- detection. Two models: one locates plates and their corners, the other turns
-- a deskewed plate into text.
--

SET @s = (SELECT IF(
    (SELECT COUNT(*) FROM INFORMATION_SCHEMA.COLUMNS WHERE table_schema = DATABASE()
     AND table_name = 'Monitors' AND column_name = 'LPREnabled') > 0,
"SELECT 'Column LPREnabled already exists in Monitors'",
"ALTER TABLE `Monitors` ADD COLUMN `LPREnabled` BOOLEAN NOT NULL DEFAULT FALSE AFTER `ObjectDetectionNMSThreshold`"
));
PREPARE stmt FROM @s; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @s = (SELECT IF(
    (SELECT COUNT(*) FROM INFORMATION_SCHEMA.COLUMNS WHERE table_schema = DATABASE()
     AND table_name = 'Monitors' AND column_name = 'LPRDetectionModel') > 0,
"SELECT 'Column LPRDetectionModel already exists in Monitors'",
"ALTER TABLE `Monitors` ADD COLUMN `LPRDetectionModel` VARCHAR(255) NOT NULL DEFAULT '' AFTER `LPREnabled`"
));
PREPARE stmt FROM @s; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @s = (SELECT IF(
    (SELECT COUNT(*) FROM INFORMATION_SCHEMA.COLUMNS WHERE table_schema = DATABASE()
     AND table_name = 'Monitors' AND column_name = 'LPRRecognitionModel') > 0,
"SELECT 'Column LPRRecognitionModel already exists in Monitors'",
"ALTER TABLE `Monitors` ADD COLUMN `LPRRecognitionModel` VARCHAR(255) NOT NULL DEFAULT '' AFTER `LPRDetectionModel`"
));
PREPARE stmt FROM @s; EXECUTE stmt; DEALLOCATE PREPARE stmt;
