DROP TABLE Sensor_Actions;

CREATE TABLE Sensor_Actions (
  `Id` integer unsigned NOT NULL auto_increment,
  `Name`      VARCHAR(255),
  `MinSensorId`  integer unsigned NOT NULL,
  `MaxSensorId`  integer unsigned NOT NULL,
  `Chain`       integer unsigned,
  `MonitorId` int(10) unsigned NOT NULL,
  `TypeId`    integer unsigned NOT NULL,
  `MinValue`  integer,
  `MaxValue`  integer,
  `Action`    VARCHAR(255),
  KEY `Sensor_Actions_SensorId_idx` (`SensorId`),
  KEY `Sensor_Actions_MonitorId_idx` (`MonitorId`),
  PRIMARY KEY (`Id`)
);


CREATE TABLE Sensor_Action_Types (
  `Id` integer unsigned NOT NULL auto_increment,
  `TypeId`  varchar(64),
  `Name`    varchar(64),
  KEY `Sensor_Action_Types_TypeId_idx` (`TypeId`),
  PRIMARY KEY (`Id`)
);

INSERT INTO Sensor_Action_Types (`TypeId`, `Name`) VALUES 
('01', 'HIGH_VB_ALARM'),
('02', 'TEMP_ALARM'),
('03', 'MAG_ALARM'),
('04', 'WIND_FILTER'),
('05', 'TREND_FILTER'),
('06', 'RADAR_FILTER'),
('07', 'NODE_MISSING_ALARM'),
('08', 'WIRECUT')
    ;
