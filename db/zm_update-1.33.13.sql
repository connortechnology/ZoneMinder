
SET @s = (SELECT IF(
    (SELECT COUNT(*) FROM INFORMATION_SCHEMA.COLUMNS WHERE table_schema = DATABASE()
      AND table_name = 'Sensor_Actions'
      AND column_name = 'MinSensorId'
    ) > 0,
    "SELECT 'Column MinSensorId already exists in Sensor_Actions'",
    "ALTER TABLE Sensor_Actions ADD `MinSensorId` INTEGER AFTER `Name`"
    ));

PREPARE stmt FROM @s;
EXECUTE stmt;

SET @s = (SELECT IF(
    (SELECT COUNT(*) FROM INFORMATION_SCHEMA.COLUMNS WHERE table_schema = DATABASE()
      AND table_name = 'Sensor_Actions'
      AND column_name = 'MaxSensorId'
    ) > 0,
    "SELECT 'Column MaxSensorId already exists in Sensor_Actions'",
    "ALTER TABLE Sensor_Actions ADD `MaxSensorId` INTEGER AFTER `MinSensorId`"
    ));

PREPARE stmt FROM @s;
EXECUTE stmt;

SET @s = (SELECT IF(
    (SELECT COUNT(*) FROM INFORMATION_SCHEMA.COLUMNS WHERE table_schema = DATABASE()
      AND table_name = 'Sensor_Actions'
      AND column_name = 'SensorId'
    ) > 0,
    "UPDATE `Sensor_Actions` SET MinSensorId=SensorId",
    "SELECT 'SensorId already gone from Sensor_Actions'"
    ));

PREPARE stmt FROM @s;
EXECUTE stmt;

SET @s = (SELECT IF(
    (SELECT COUNT(*) FROM INFORMATION_SCHEMA.COLUMNS WHERE table_schema = DATABASE()
      AND table_name = 'Sensor_Actions'
      AND column_name = 'SensorId'
    ) > 0,
    "UPDATE `Sensor_Actions` SET MaxSensorId=SensorId",
    "SELECT 'SensorId already gone from Sensor_Actions'"
    ));

PREPARE stmt FROM @s;
EXECUTE stmt;
SET @s = (SELECT IF(
    (SELECT COUNT(*) FROM INFORMATION_SCHEMA.COLUMNS WHERE table_schema = DATABASE()
      AND table_name = 'Sensor_Actions'
      AND column_name = 'SensorId'
    ) > 0,
    "ALTER TABLE Sensor_Actions DROP COLUMN SensorId",
    "SELECT 'SensorId already gone from Sensor_Actions'"
    ));

PREPARE stmt FROM @s;
EXECUTE stmt;
