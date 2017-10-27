--
-- Add StorageId column to Monitors
--

SET @s = (SELECT IF(
    (SELECT COUNT(*)
    FROM INFORMATION_SCHEMA.COLUMNS
    WHERE table_name = 'Filters'
    AND table_schema = DATABASE()
    AND column_name = 'UpdateDiskSpace'
    ) > 0,
"SELECT 'Column UpdateDiskSpace exists in Filters'",
"ALTER TABLE Filters ADD `UpdateDiskSpace` tinyint(3) unsigned NOT NULL default '0' AFTER `AutoDelete`"
));

PREPARE stmt FROM @s;
EXECUTE stmt;

