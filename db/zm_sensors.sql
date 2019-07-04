DROP TABLE Sensors;

CREATE TABLE Sensors (
  `Id` integer unsigned NOT NULL auto_increment,
  `SensorId`    VARCHAR(255),
  `SensorServerId`  integer unsigned,
  `Chain`       integer unsigned,
  `Name`    VARCHAR(255),
  `MinValue`  INTEGER,
  `MaxValue`  INTEGER,
  KEY `Sensors_SensorId_idx` (`SensorId`),
  KEY `Sensors_ServerId_idx` (`SensorServerId`),
  PRIMARY KEY (`Id`)
);

INSERT INTO Sensors (SensorId, Name, SensorServerId) values ('01','01',1), ('02','02',1),('03','03',1);
