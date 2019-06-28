
SET @s = (SELECT IF(
    (SELECT COUNT(*) FROM INFORMATION_SCHEMA.COLUMNS WHERE table_schema = DATABASE()
      AND table_name = 'Sensor_Servers'
      AND column_name = 'TypeId'
    ) > 0,
    "SELECT 'Column TypeId already exists in Sensor_Servers'",
    "ALTER TABLE Sensor_Servers ADD `TypeId`  integer AFTER `Name`"
    ));

PREPARE stmt FROM @s;
EXECUTE stmt;

CREATE TABLE Sensor_Server_Types (
  `Id` integer unsigned NOT NULL auto_increment,
  `Name`    VARCHAR(255),
  `Chains`  VARCHAR(255),
  `MaxSensors`  integer,
  PRIMARY KEY (`Id`)
);

INSERT INTO Sensor_Server_Types (`Name`,`Chains`,`MaxSensors`) VALUES ('Node','1:Left,2:Right', 250);

ALTER TABLE Sensors add Chain integer unsigned after SensorServerId;
