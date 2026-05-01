--
-- Widen Monitors.ObjectDetection so values longer than 7 chars fit.
--
-- Historically this column was an ENUM whose entries grew over time
-- (none/quadra/speedai/uvicorn -> +mx_accl). It was changed to VARCHAR(8) so
-- new backends could be added without a schema change, but VARCHAR(8) only
-- just fits the current backend names and silently truncates anything longer
-- (MySQL warning 1265). Bump to VARCHAR(16) for headroom and convert any
-- still-ENUM column from old upgrade paths.
--
-- Idempotent: re-running is a no-op once the column is already varchar(16).
--

SET @col_type = (
  SELECT COLUMN_TYPE FROM INFORMATION_SCHEMA.COLUMNS
   WHERE TABLE_SCHEMA = DATABASE()
     AND TABLE_NAME = 'Monitors'
     AND COLUMN_NAME = 'ObjectDetection'
);

SET @s = IF(@col_type = 'varchar(16)',
  "SELECT 'Monitors.ObjectDetection already varchar(16)'",
  "ALTER TABLE `Monitors` MODIFY `ObjectDetection` VARCHAR(16) NOT NULL DEFAULT 'none'");

PREPARE stmt FROM @s;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;
