CREATE TABLE Sensor_Server_Types (
  `Id` integer unsigned NOT NULL auto_increment,
  `Name`    VARCHAR(255),
  `Chains`  VARCHAR(255),
  `MaxSensors`  integer,
  PRIMARY KEY (`Id`)
);

INSERT INTO Sensor_Server_Types (`Name`,`Chains`,`MaxSensors`) VALUES ('Node','1:Left,2:Right', 250);
