
SET @s = (SELECT IF(
    (SELECT COUNT(*) FROM INFORMATION_SCHEMA.COLUMNS WHERE table_schema = DATABASE()
      AND table_name = 'Servers'
      AND column_name = 'zmfilter'
    ) > 0,
    "SELECT 'Column zmfilter already exists in Servers'",
    "ALTER TABLE Servers ADD `zmfilter` BOOLEAN NOT NULL DEFAULT FALSE AFTER `zmtrigger`"
    ));

PREPARE stmt FROM @s;
EXECUTE stmt;
