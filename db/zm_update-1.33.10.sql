--
-- This adds Manufacturers and Models
--

SET @s = (SELECT IF(
    (SELECT COUNT(*)
      FROM INFORMATION_SCHEMA.TABLES
      WHERE table_name = 'Sensor_Servers'
      AND table_schema = DATABASE()
    ) > 0,
    "SELECT 'Sensor_Servers table exists'",
    "
CREATE TABLE Sensor_Servers (
  `Id` integer unsigned NOT NULL auto_increment,
  `Name`    VARCHAR(255),
  `Url`    VARCHAR(255),
  `PollFrequency` INTEGER UNSIGNED,
  PRIMARY KEY (`Id`)
);
    "
  ));

PREPARE stmt FROM @s;
EXECUTE stmt;

SET @s = (SELECT IF(
    (SELECT COUNT(*)
      FROM INFORMATION_SCHEMA.TABLES
      WHERE table_name = 'Sensors'
      AND table_schema = DATABASE()
    ) > 0,
    "SELECT 'Sensors table exists'",
    "
CREATE TABLE Sensors (
  `Id` integer unsigned NOT NULL auto_increment,
  `SensorId`    VARCHAR(255),
  `SensorServerId`  integer unsigned,
  `Name`    VARCHAR(255),
  KEY `Sensors_SensorId_idx` (`SensorId`),
  KEY `Sensors_ServerId_idx` (`SensorServerId`),
  PRIMARY KEY (`Id`)
);
    "
));

PREPARE stmt FROM @s;
EXECUTE stmt;


SET @s = (SELECT IF(
    (SELECT COUNT(*)
      FROM INFORMATION_SCHEMA.TABLES
      WHERE table_name = 'Sensor_Actions'
      AND table_schema = DATABASE()
    ) > 0,
    "SELECT 'Sensor Actions table exists'",
    "
CREATE TABLE Sensor_Actions (
  `Id` integer unsigned NOT NULL auto_increment,
  `Name`      VARCHAR(255),
  `SensorId`  integer unsigned NOT NULL,
  `MonitorId` int(10) unsigned NOT NULL,
  `TypeId`    integer unsigned NOT NULL,
  `MinValue`  integer,
  `MaxValue`  integer,
  `Action`    VARCHAR(255),
  KEY `Sensor_Actions_SensorId_idx` (`SensorId`),
  KEY `Sensor_Actions_MonitorId_idx` (`MonitorId`),
  PRIMARY KEY (`Id`)
);

    "
));

PREPARE stmt FROM @s;
EXECUTE stmt;
