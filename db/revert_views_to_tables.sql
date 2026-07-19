--
-- Revert Event Summary views/SWR back to trigger-maintained tables
--

DELIMITER //

-- 1. Drop SWR infrastructure
DROP EVENT IF EXISTS `Event_Summaries_Refresh_Event`//
DROP PROCEDURE IF EXISTS `Refresh_Summaries_SWR`//
DROP TABLE IF EXISTS `Event_Summaries_Metadata`//
DROP TABLE IF EXISTS `Event_Summaries_New`//
DROP TABLE IF EXISTS `Event_Summaries_Old`//

-- 2. Drop views (IF EXISTS handles case where they're already tables)
DROP VIEW IF EXISTS `Events_Hour`//
DROP VIEW IF EXISTS `Events_Day`//
DROP VIEW IF EXISTS `Events_Week`//
DROP VIEW IF EXISTS `Events_Month`//
DROP VIEW IF EXISTS `Events_Archived`//
DROP VIEW IF EXISTS `VIEW_Event_Summaries`//

-- 3. Drop Event_Summaries (the SWR snapshot table)
DROP TABLE IF EXISTS `Event_Summaries`//

-- 4. Recreate summary tables

CREATE TABLE `Events_Hour` (
  `EventId` BIGINT unsigned NOT NULL,
  `MonitorId` int(10) unsigned NOT NULL,
  `StartDateTime` datetime default NULL,
  `DiskSpace`   bigint default NULL,
  PRIMARY KEY (`EventId`),
  KEY `Events_Hour_MonitorId_idx` (`MonitorId`),
  KEY `Events_Hour_StartDateTime_idx` (`StartDateTime`)
) ENGINE=InnoDB//

CREATE TABLE `Events_Day` (
  `EventId` BIGINT unsigned NOT NULL,
  `MonitorId` int(10) unsigned NOT NULL,
  `StartDateTime` datetime default NULL,
  `DiskSpace`   bigint default NULL,
  PRIMARY KEY (`EventId`),
  KEY `Events_Day_MonitorId_idx` (`MonitorId`),
  KEY `Events_Day_StartDateTime_idx` (`StartDateTime`)
) ENGINE=InnoDB//

CREATE TABLE `Events_Week` (
  `EventId` BIGINT unsigned NOT NULL,
  `MonitorId` int(10) unsigned NOT NULL,
  `StartDateTime` datetime default NULL,
  `DiskSpace`   bigint default NULL,
  PRIMARY KEY (`EventId`),
  KEY `Events_Week_MonitorId_idx` (`MonitorId`),
  KEY `Events_Week_StartDateTime_idx` (`StartDateTime`)
) ENGINE=InnoDB//

CREATE TABLE `Events_Month` (
  `EventId` BIGINT unsigned NOT NULL,
  `MonitorId` int(10) unsigned NOT NULL,
  `StartDateTime` datetime default NULL,
  `DiskSpace`   bigint default NULL,
  PRIMARY KEY (`EventId`),
  KEY `Events_Month_MonitorId_idx` (`MonitorId`),
  KEY `Events_Month_StartDateTime_idx` (`StartDateTime`)
) ENGINE=InnoDB//

CREATE TABLE `Events_Archived` (
  `EventId` BIGINT unsigned NOT NULL,
  `MonitorId` int(10) unsigned NOT NULL,
  `DiskSpace`   bigint default NULL,
  PRIMARY KEY (`EventId`),
  KEY `Events_Archived_MonitorId_idx` (`MonitorId`)
) ENGINE=InnoDB//

CREATE TABLE `Event_Summaries` (
  `MonitorId` int(10) unsigned NOT NULL,
  `TotalEvents` int(10) default NULL,
  `TotalEventDiskSpace` bigint default NULL,
  `HourEvents` int(10) default NULL,
  `HourEventDiskSpace` bigint default NULL,
  `DayEvents` int(10) default NULL,
  `DayEventDiskSpace` bigint default NULL,
  `WeekEvents` int(10) default NULL,
  `WeekEventDiskSpace` bigint default NULL,
  `MonthEvents` int(10) default NULL,
  `MonthEventDiskSpace` bigint default NULL,
  `ArchivedEvents` int(10) default NULL,
  `ArchivedEventDiskSpace` bigint default NULL,
  PRIMARY KEY (`MonitorId`)
) ENGINE=InnoDB//

-- 5. Populate summary tables from Events

INSERT INTO `Events_Hour` (EventId, MonitorId, StartDateTime, DiskSpace)
  SELECT Id, MonitorId, StartDateTime, DiskSpace FROM Events
  WHERE StartDateTime >= (NOW() - INTERVAL 1 HOUR)//

INSERT INTO `Events_Day` (EventId, MonitorId, StartDateTime, DiskSpace)
  SELECT Id, MonitorId, StartDateTime, DiskSpace FROM Events
  WHERE StartDateTime >= (NOW() - INTERVAL 1 DAY)//

INSERT INTO `Events_Week` (EventId, MonitorId, StartDateTime, DiskSpace)
  SELECT Id, MonitorId, StartDateTime, DiskSpace FROM Events
  WHERE StartDateTime >= (NOW() - INTERVAL 7 DAY)//

INSERT INTO `Events_Month` (EventId, MonitorId, StartDateTime, DiskSpace)
  SELECT Id, MonitorId, StartDateTime, DiskSpace FROM Events
  WHERE StartDateTime >= (NOW() - INTERVAL 1 MONTH)//

INSERT INTO `Events_Archived` (EventId, MonitorId, DiskSpace)
  SELECT Id, MonitorId, DiskSpace FROM Events
  WHERE Archived = 1//

INSERT INTO `Event_Summaries` (MonitorId, HourEvents, HourEventDiskSpace, DayEvents, DayEventDiskSpace, WeekEvents, WeekEventDiskSpace, MonthEvents, MonthEventDiskSpace, ArchivedEvents, ArchivedEventDiskSpace, TotalEvents, TotalEventDiskSpace)
  SELECT
    MonitorId,
    COUNT(CASE WHEN StartDateTime >= (NOW() - INTERVAL 1 HOUR) THEN 1 END),
    COALESCE(SUM(CASE WHEN StartDateTime >= (NOW() - INTERVAL 1 HOUR) THEN DiskSpace ELSE 0 END), 0),
    COUNT(CASE WHEN StartDateTime >= (NOW() - INTERVAL 1 DAY) THEN 1 END),
    COALESCE(SUM(CASE WHEN StartDateTime >= (NOW() - INTERVAL 1 DAY) THEN DiskSpace ELSE 0 END), 0),
    COUNT(CASE WHEN StartDateTime >= (NOW() - INTERVAL 7 DAY) THEN 1 END),
    COALESCE(SUM(CASE WHEN StartDateTime >= (NOW() - INTERVAL 7 DAY) THEN DiskSpace ELSE 0 END), 0),
    COUNT(CASE WHEN StartDateTime >= (NOW() - INTERVAL 1 MONTH) THEN 1 END),
    COALESCE(SUM(CASE WHEN StartDateTime >= (NOW() - INTERVAL 1 MONTH) THEN DiskSpace ELSE 0 END), 0),
    COUNT(CASE WHEN Archived = 1 THEN 1 END),
    COALESCE(SUM(CASE WHEN Archived = 1 THEN DiskSpace ELSE 0 END), 0),
    COUNT(Id),
    COALESCE(SUM(DiskSpace), 0)
  FROM Events
  GROUP BY MonitorId//

-- 6. Recreate triggers

DROP TRIGGER IF EXISTS Events_Hour_delete_trigger//
CREATE TRIGGER Events_Hour_delete_trigger BEFORE DELETE ON Events_Hour
FOR EACH ROW BEGIN
  UPDATE Event_Summaries SET
  HourEvents = GREATEST(COALESCE(HourEvents,1)-1,0),
  HourEventDiskSpace=GREATEST(COALESCE(HourEventDiskSpace,0)-COALESCE(OLD.DiskSpace,0),0)
  WHERE Event_Summaries.MonitorId=OLD.MonitorId;
END;
//

DROP TRIGGER IF EXISTS Events_Hour_update_trigger//
CREATE TRIGGER Events_Hour_update_trigger AFTER UPDATE ON Events_Hour
FOR EACH ROW
  BEGIN
    declare diff BIGINT default 0;
    set diff = COALESCE(NEW.DiskSpace,0) - COALESCE(OLD.DiskSpace,0);
    IF ( diff ) THEN
      IF ( NEW.MonitorID != OLD.MonitorID ) THEN
        UPDATE Event_Summaries SET HourEventDiskSpace=GREATEST(COALESCE(HourEventDiskSpace,0)-COALESCE(OLD.DiskSpace,0),0) WHERE Event_Summaries.MonitorId=OLD.MonitorId;
        UPDATE Event_Summaries SET HourEventDiskSpace=COALESCE(HourEventDiskSpace,0)+COALESCE(NEW.DiskSpace,0) WHERE Event_Summaries.MonitorId=NEW.MonitorId;
      ELSE
        UPDATE Event_Summaries SET HourEventDiskSpace=COALESCE(HourEventDiskSpace,0)+diff WHERE Event_Summaries.MonitorId=NEW.MonitorId;
      END IF;
    END IF;
  END;
//

DROP TRIGGER IF EXISTS Events_Day_delete_trigger//
CREATE TRIGGER Events_Day_delete_trigger BEFORE DELETE ON Events_Day
FOR EACH ROW BEGIN
  UPDATE Event_Summaries SET
  DayEvents = GREATEST(COALESCE(DayEvents,1)-1,0),
  DayEventDiskSpace=GREATEST(COALESCE(DayEventDiskSpace,0)-COALESCE(OLD.DiskSpace,0),0)
  WHERE Event_Summaries.MonitorId=OLD.MonitorId;
END;
//

DROP TRIGGER IF EXISTS Events_Day_update_trigger//
CREATE TRIGGER Events_Day_update_trigger AFTER UPDATE ON Events_Day
FOR EACH ROW
  BEGIN
    declare diff BIGINT default 0;
    set diff = COALESCE(NEW.DiskSpace,0) - COALESCE(OLD.DiskSpace,0);
    IF ( diff ) THEN
      IF ( NEW.MonitorID != OLD.MonitorID ) THEN
        UPDATE Event_Summaries SET DayEventDiskSpace=GREATEST(COALESCE(DayEventDiskSpace,0)-COALESCE(OLD.DiskSpace,0),0) WHERE Event_Summaries.MonitorId=OLD.MonitorId;
        UPDATE Event_Summaries SET DayEventDiskSpace=COALESCE(DayEventDiskSpace,0)+COALESCE(NEW.DiskSpace,0) WHERE Event_Summaries.MonitorId=NEW.MonitorId;
      ELSE
        UPDATE Event_Summaries SET DayEventDiskSpace=GREATEST(COALESCE(DayEventDiskSpace,0)+diff,0) WHERE Event_Summaries.MonitorId=NEW.MonitorId;
      END IF;
    END IF;
  END;
//

DROP TRIGGER IF EXISTS Events_Week_delete_trigger//
CREATE TRIGGER Events_Week_delete_trigger BEFORE DELETE ON Events_Week
FOR EACH ROW BEGIN
  UPDATE Event_Summaries SET
  WeekEvents = GREATEST(COALESCE(WeekEvents,1)-1,0),
  WeekEventDiskSpace=GREATEST(COALESCE(WeekEventDiskSpace,0)-COALESCE(OLD.DiskSpace,0),0)
  WHERE Event_Summaries.MonitorId=OLD.MonitorId;
END;
//

DROP TRIGGER IF EXISTS Events_Week_update_trigger//
CREATE TRIGGER Events_Week_update_trigger AFTER UPDATE ON Events_Week
FOR EACH ROW
  BEGIN
    declare diff BIGINT default 0;
    set diff = COALESCE(NEW.DiskSpace,0) - COALESCE(OLD.DiskSpace,0);
    IF ( diff ) THEN
      IF ( NEW.MonitorID != OLD.MonitorID ) THEN
        UPDATE Event_Summaries SET WeekEventDiskSpace=GREATEST(COALESCE(WeekEventDiskSpace,0)-COALESCE(OLD.DiskSpace,0),0) WHERE Event_Summaries.MonitorId=OLD.MonitorId;
        UPDATE Event_Summaries SET WeekEventDiskSpace=COALESCE(WeekEventDiskSpace,0)+COALESCE(NEW.DiskSpace,0) WHERE Event_Summaries.MonitorId=NEW.MonitorId;
      ELSE
        UPDATE Event_Summaries SET WeekEventDiskSpace=GREATEST(COALESCE(WeekEventDiskSpace,0)+diff,0) WHERE Event_Summaries.MonitorId=NEW.MonitorId;
      END IF;
    END IF;
  END;
//

DROP TRIGGER IF EXISTS Events_Month_delete_trigger//
CREATE TRIGGER Events_Month_delete_trigger BEFORE DELETE ON Events_Month
FOR EACH ROW BEGIN
  UPDATE Event_Summaries SET
  MonthEvents = GREATEST(COALESCE(MonthEvents,1)-1,0),
  MonthEventDiskSpace=GREATEST(COALESCE(MonthEventDiskSpace,0)-COALESCE(OLD.DiskSpace,0),0)
  WHERE Event_Summaries.MonitorId=OLD.MonitorId;
END;
//

DROP TRIGGER IF EXISTS Events_Month_update_trigger//
CREATE TRIGGER Events_Month_update_trigger AFTER UPDATE ON Events_Month
FOR EACH ROW
  BEGIN
    declare diff BIGINT default 0;
    set diff = COALESCE(NEW.DiskSpace,0) - COALESCE(OLD.DiskSpace,0);
    IF ( diff ) THEN
      IF ( NEW.MonitorID != OLD.MonitorID ) THEN
        UPDATE Event_Summaries SET MonthEventDiskSpace=GREATEST(COALESCE(MonthEventDiskSpace,0)-COALESCE(OLD.DiskSpace),0) WHERE Event_Summaries.MonitorId=OLD.MonitorId;
        UPDATE Event_Summaries SET MonthEventDiskSpace=COALESCE(MonthEventDiskSpace,0)+COALESCE(NEW.DiskSpace) WHERE Event_Summaries.MonitorId=NEW.MonitorId;
      ELSE
        UPDATE Event_Summaries SET MonthEventDiskSpace=GREATEST(COALESCE(MonthEventDiskSpace,0)+diff,0) WHERE Event_Summaries.MonitorId=NEW.MonitorId;
      END IF;
    END IF;
  END;
//

DROP TRIGGER IF EXISTS event_update_trigger//
CREATE TRIGGER event_update_trigger AFTER UPDATE ON Events
FOR EACH ROW
BEGIN
  declare diff BIGINT default 0;
  set diff = COALESCE(NEW.DiskSpace,0) - COALESCE(OLD.DiskSpace,0);

  IF ( diff ) THEN
    UPDATE Events_Hour SET DiskSpace=NEW.DiskSpace WHERE EventId=NEW.Id;
    UPDATE Events_Day SET DiskSpace=NEW.DiskSpace WHERE EventId=NEW.Id;
    UPDATE Events_Week SET DiskSpace=NEW.DiskSpace WHERE EventId=NEW.Id;
    UPDATE Events_Month SET DiskSpace=NEW.DiskSpace WHERE EventId=NEW.Id;
    UPDATE Event_Summaries
      SET
        TotalEventDiskSpace = GREATEST(COALESCE(TotalEventDiskSpace,0) - COALESCE(OLD.DiskSpace,0) + COALESCE(NEW.DiskSpace,0),0)
      WHERE Event_Summaries.MonitorId=OLD.MonitorId;
  END IF;

  IF ( NEW.Archived != OLD.Archived ) THEN
    IF ( NEW.Archived ) THEN
      INSERT INTO Events_Archived (EventId,MonitorId,DiskSpace) VALUES (NEW.Id,NEW.MonitorId,NEW.DiskSpace);
      INSERT INTO Event_Summaries (MonitorId,ArchivedEvents,ArchivedEventDiskSpace) VALUES (NEW.MonitorId,1,NEW.DiskSpace) ON DUPLICATE KEY
        UPDATE ArchivedEvents = COALESCE(ArchivedEvents,0)+1, ArchivedEventDiskSpace = COALESCE(ArchivedEventDiskSpace,0) + COALESCE(NEW.DiskSpace,0);
    ELSEIF ( OLD.Archived ) THEN
      DELETE FROM Events_Archived WHERE EventId=OLD.Id;
      UPDATE Event_Summaries
        SET
          ArchivedEvents = GREATEST(COALESCE(ArchivedEvents,0)-1,0),
          ArchivedEventDiskSpace = GREATEST(COALESCE(ArchivedEventDiskSpace,0) - COALESCE(OLD.DiskSpace,0),0)
        WHERE Event_Summaries.MonitorId=OLD.MonitorId;
    ELSE
      IF ( OLD.DiskSpace != NEW.DiskSpace ) THEN
        UPDATE Events_Archived SET DiskSpace=NEW.DiskSpace WHERE EventId=NEW.Id;
        UPDATE Event_Summaries SET
          ArchivedEventDiskSpace = GREATEST(COALESCE(ArchivedEventDiskSpace,0) - COALESCE(OLD.DiskSpace,0) + COALESCE(NEW.DiskSpace,0),0)
          WHERE Event_Summaries.MonitorId=OLD.MonitorId;
      END IF;
    END IF;
  ELSEIF ( NEW.Archived AND diff ) THEN
    UPDATE Events_Archived SET DiskSpace=NEW.DiskSpace WHERE EventId=NEW.Id;
  END IF;

END;
//

DROP TRIGGER IF EXISTS event_delete_trigger//
CREATE TRIGGER event_delete_trigger BEFORE DELETE ON Events
FOR EACH ROW
BEGIN
  DELETE FROM Events_Hour WHERE EventId=OLD.Id;
  DELETE FROM Events_Day WHERE EventId=OLD.Id;
  DELETE FROM Events_Week WHERE EventId=OLD.Id;
  DELETE FROM Events_Month WHERE EventId=OLD.Id;
  IF ( OLD.Archived ) THEN
    DELETE FROM Events_Archived WHERE EventId=OLD.Id;
    UPDATE Event_Summaries SET
      ArchivedEvents = GREATEST(COALESCE(ArchivedEvents,1) - 1,0),
      ArchivedEventDiskSpace = GREATEST(COALESCE(ArchivedEventDiskSpace,0) - COALESCE(OLD.DiskSpace,0),0),
      TotalEvents = GREATEST(COALESCE(TotalEvents,1) - 1,0),
      TotalEventDiskSpace = GREATEST(COALESCE(TotalEventDiskSpace,0) - COALESCE(OLD.DiskSpace,0),0)
      WHERE Event_Summaries.MonitorId=OLD.MonitorId;
  ELSE
    UPDATE Event_Summaries SET
    TotalEvents = GREATEST(COALESCE(TotalEvents,1)-1,0),
    TotalEventDiskSpace=GREATEST(COALESCE(TotalEventDiskSpace,0)-COALESCE(OLD.DiskSpace,0),0)
    WHERE Event_Summaries.MonitorId=OLD.MonitorId;
  END IF;
END;
//

DELIMITER ;
