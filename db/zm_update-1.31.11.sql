--
-- Add UpdateDiskSpace column to Filters
--

SET @s = (SELECT IF(
    (SELECT COUNT(*) FROM INFORMATION_SCHEMA.COLUMNS WHERE table_schema = DATABASE()
    AND table_name = 'Filters'
    AND column_name = 'UpdateDiskSpace'
    ) > 0,
"SELECT 'Column UpdateDiskSpace exists in Filters'",
"ALTER TABLE Filters ADD `UpdateDiskSpace` tinyint(3) unsigned NOT NULL default '0' AFTER `AutoDelete`"
));

PREPARE stmt FROM @s;
EXECUTE stmt;
