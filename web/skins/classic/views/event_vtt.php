<?php
//
// ZoneMinder web event view file, $Date$, $Revision$
// Copyright (C) 2001-2008 Philip Coombes
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
//

if ( !canView('Events') ) {
  $view = 'error';
  return;
}

$eid = validInt($_REQUEST['eid']);
header("Content-Type:text/vtt;charset=utf-8");
echo "WEBVTT ZoneMinder\n\n";
$Event = new Event($eid);
  $start_seconds = strtotime($Event->StartTime());
  $end_seconds = strtotime($Event->EndTime());
  $duration = $end_seconds - $start_seconds;
  for ( $time = 0; $time < $duration; $time ++ ) {
    echo format_duration($time).'.000 --> '.format_duration($time+1). ".000\n" . date('Y-m-d H:i:s',$start_seconds+$time)."\n\n";
  }
?>
