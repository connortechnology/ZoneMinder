DROP TABLE Sensor_Actions;

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

INSERT INTO Sensor_Actions (`SensorId`, `MonitorId`, `TypeId`, `MinValue`, `MaxValue`, `Action`) VALUES
(3, 1, 2, NULL, NULL, 'Preset 1');
