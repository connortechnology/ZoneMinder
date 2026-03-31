<?php
//
// ZoneMinder HLS manifest generator for segmented event video
// Generates an m3u8 playlist from Event_Video_Segments table
//

if (!canView('Events')) {
  $view = 'error';
  return;
}

ob_end_clean();
require_once('includes/Event.php');

if (empty($_REQUEST['eid'])) {
  header('HTTP/1.1 400 Bad Request');
  die('Missing eid parameter');
}

$Event = new ZM\Event($_REQUEST['eid']);
if (!$Event->Id()) {
  header('HTTP/1.1 404 Not Found');
  die('Event not found');
}

$segments = dbFetchAll(
  'SELECT SegmentIndex, Filename, StartDelta, Duration, Bytes'
  . ' FROM Event_Video_Segments WHERE EventId = ? ORDER BY SegmentIndex',
  NULL, array($Event->Id())
);

if (!$segments || count($segments) == 0) {
  // No segments — fall back to single-file mode by redirecting to view_video
  header('Location: ?view=view_video&eid='.$Event->Id());
  exit;
}

// Calculate max segment duration for EXT-X-TARGETDURATION (must be integer, rounded up)
$max_duration = 0;
foreach ($segments as $seg) {
  if ($seg['Duration'] > $max_duration) {
    $max_duration = $seg['Duration'];
  }
}
$target_duration = (int)ceil($max_duration);
if ($target_duration < 1) $target_duration = 10;

header('Content-Type: application/vnd.apple.mpegurl');
header('Cache-Control: no-cache');

$Server = $Event->Server();
$base_url = $Server->PathToIndex();

// Build auth args
$auth_query = '';
if (ZM_OPT_USE_AUTH) {
  if (ZM_AUTH_RELAY == 'hashed') {
    $auth_query = '&auth=' . generateAuthHash(ZM_AUTH_HASH_IPS);
  } else if (ZM_AUTH_RELAY == 'plain') {
    $auth_query = '&user=' . $_SESSION['username'] . '&pass=' . $_SESSION['password'];
  } else if (ZM_AUTH_RELAY == 'none') {
    $auth_query = '&user=' . $_SESSION['username'];
  }
}

// Build segment URL helper
$make_segment_url = function($filename) use ($base_url, $Event, $auth_query) {
  return $base_url . '?view=view_video&eid=' . $Event->Id()
    . '&file=' . urlencode($filename) . $auth_query;
};

// Determine init segment size from first segment (ftyp + moov boxes)
$first_seg_path = $Event->Path() . '/' . basename($segments[0]['Filename']);
$init_size = 0;
$fh = @fopen($first_seg_path, 'rb');
if ($fh) {
  // Read ftyp box size (big-endian uint32 at offset 0)
  $data = fread($fh, 4);
  if ($data !== false && strlen($data) == 4) {
    $ftyp_size = unpack('N', $data)[1];
    // Seek to moov box and read its size
    fseek($fh, $ftyp_size);
    $data = fread($fh, 4);
    if ($data !== false && strlen($data) == 4) {
      $moov_size = unpack('N', $data)[1];
      $init_size = $ftyp_size + $moov_size;
    }
  }
  fclose($fh);
}

echo "#EXTM3U\n";
echo "#EXT-X-VERSION:7\n";
echo "#EXT-X-TARGETDURATION:$target_duration\n";
echo "#EXT-X-MEDIA-SEQUENCE:0\n";
echo "#EXT-X-PLAYLIST-TYPE:VOD\n";
echo "#EXT-X-INDEPENDENT-SEGMENTS\n";

// fMP4 init segment (ftyp+moov from first segment)
$init_url = $make_segment_url($segments[0]['Filename']);
if ($init_size > 0) {
  echo "#EXT-X-MAP:URI=\"$init_url\",BYTERANGE=\"$init_size@0\"\n";
} else {
  echo "#EXT-X-MAP:URI=\"$init_url\"\n";
}

foreach ($segments as $seg) {
  $duration = sprintf('%.3f', $seg['Duration']);
  $segment_url = $make_segment_url($seg['Filename']);
  echo "#EXTINF:$duration,\n";
  echo "$segment_url\n";
}

echo "#EXT-X-ENDLIST\n";
exit;
