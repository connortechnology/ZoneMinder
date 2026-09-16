--
-- Normalise Monitors.ObjectDetection to lower case.
--
-- The column is a VARCHAR and the C++ side matches it exactly: "none",
-- "quadra", "uvicorn", "mx_accl", "openvino". Older rows hold "None" with a
-- capital, which falls through to the unsupported branch and logs
--
--   Unsupported value for ObjectDetection: None
--
-- once per monitor load. The monitor still ends up with detection off, so this
-- is noise rather than a behaviour change, but it is noise on every start.
--
-- Unconditional rather than filtered: MySQL's default collation is case
-- insensitive, so `WHERE ObjectDetection <> LOWER(ObjectDetection)` matches
-- nothing at all and the migration would silently do nothing. LOWER() on an
-- already-lower value is a no-op, so running this twice is harmless.
--

UPDATE `Monitors` SET `ObjectDetection` = LOWER(`ObjectDetection`);
