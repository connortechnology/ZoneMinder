DROP TABLE Sensor_Servers;

CREATE TABLE Sensor_Servers (
  `Id` integer unsigned NOT NULL auto_increment,
  `Name`    VARCHAR(255),
  `Url`    VARCHAR(255),
  `PollFrequency` INTEGER UNSIGNED,
  PRIMARY KEY (`Id`)
);

INSERT INTO Sensor_Servers (`Name`,`Url`,`PollFrequency`) VALUES ('Node1','http://www.texonglobal.com/astatus.xml', 3);