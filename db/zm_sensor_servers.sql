DROP TABLE Sensor_Servers;

CREATE TABLE Sensor_Servers (
  `Id` integer unsigned NOT NULL auto_increment,
  `Name`    VARCHAR(255),
  `TypeId`  integer,
  `Url`    VARCHAR(255),
  `PollFrequency` INTEGER UNSIGNED,
  `Chains`  VARCHAR(255),
  PRIMARY KEY (`Id`)
);

INSERT INTO Sensor_Servers (`Name`,`Url`,`PollFrequency`) VALUES ('Node1','http://www.texonglobal.com/astatus.xml', 3, '1:Left,2:Right');
