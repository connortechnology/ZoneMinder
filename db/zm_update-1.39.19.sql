--
-- Canonicalise Monitors.ObjectDetection to lowercase.
--
-- The column's values are all lowercase ('none', 'quadra', 'uvicorn',
-- 'openvino', ...) but the monitor edit UI offered the off setting as 'None'
-- with a capital N, and the column has been a plain VARCHAR since 1.37.69, so
-- that spelling was stored verbatim. zmc only recognised 'none' and logged
-- "Unsupported value for ObjectDetection: None" for every such monitor on
-- load, the console's javascript reported it as actively object detecting,
-- and the monitor edit page left the model/threshold fields showing.
--
-- Unconditional LOWER() rather than a WHERE clause: the default collation is
-- case-insensitive, so `WHERE ObjectDetection != LOWER(ObjectDetection)`
-- would never match. This is idempotent as written.
--

UPDATE `Monitors` SET `ObjectDetection` = LOWER(`ObjectDetection`);
