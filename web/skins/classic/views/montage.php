<?php
//
// ZoneMinder web montage view file, $Date$, $Revision$
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

if ( !canView('Stream') ) {
  $view = 'error';
  return;
}

$showControl = false;
$showZones = false;
if ( isset( $_REQUEST['showZones'] ) ) {
  if ( $_REQUEST['showZones'] == 1 ) {
    $showZones = true;
  }
}
$widths = array( 
  ''  => 'auto',
  160 => 160,
  320 => 320,
  352 => 352,
  640 => 640,
  1280 => 1280 );

$heights = array( 
  ''  => 'auto',
  240 => 240,
  480 => 480,
);

$scale = '100';   # actual

if ( isset( $_REQUEST['scale'] ) ) {
  $scale = validInt($_REQUEST['scale']);
  Logger::Debug("Setting scale from request to $scale");
} else if ( isset( $_COOKIE['zmMontageScale'] ) ) {
  $scale = $_COOKIE['zmMontageScale'];
  Logger::Debug("Setting scale from cookie to $scale");
}

if ( ! $scale ) 
  $scale = 100;

$focusWindow = true;

$layouts = array(
  'montage_freeform.css' => translate('MtgDefault'),
  'montage_2wide.css' => translate('Mtg2widgrd'),
  'montage_3wide.css' => translate('Mtg3widgrd'),
  'montage_4wide.css' => translate('Mtg4widgrd'),
  'montage_3wide50enlarge.css' => translate('Mtg3widgrx'),
);

$layout = '';
if ( isset($_COOKIE['zmMontageLayout']) )
  $layout = $_COOKIE['zmMontageLayout'];

$options = array();
if ( isset($_COOKIE['zmMontageWidth']) and $_COOKIE['zmMontageWidth'] )
  $options['width'] = $_COOKIE['zmMontageWidth'];
else
  $options['width'] = '';
if ( isset($_COOKIE['zmMontageHeight']) and $_COOKIE['zmMontageHeight'] )
  $options['height'] = $_COOKIE['zmMontageHeight'];
else
  $options['height'] = '';
if ( $scale ) 
  $options['scale'] = $scale;

ob_start();
# This will end up with the group_id of the deepest selection
$group_id = Group::get_group_dropdowns();
$group_dropdowns = ob_get_contents();
ob_end_clean();

$groupSql = Group::get_group_sql( $group_id );

$monitor_id = 0;
if ( isset( $_REQUEST['monitor_id'] ) ) {
  $monitor_id = $_REQUEST['monitor_id'];
} else if ( isset($_COOKIE['zmMonitorId']) ) {
  $monitor_id = $_COOKIE['zmMonitorId'];
}

$monitors = array();
$monitors_dropdown = array( '' => 'All' );
$sql = "SELECT * FROM Monitors WHERE Function != 'None'";
if ( $groupSql ) { $sql .= ' AND ' . $groupSql; };
if ( $monitor_id ) { $sql .= ' AND Id='.$monitor_id; };

$sql .= ' ORDER BY Sequence';
foreach( dbFetchAll( $sql ) as $row ) {
  if ( !visibleMonitor( $row['Id'] ) ) {
    continue;
  }

  $row['Scale'] = $scale;
  $row['PopupScale'] = reScale( SCALE_BASE, $row['DefaultScale'], ZM_WEB_DEFAULT_SCALE );

  if ( ZM_OPT_CONTROL && $row['ControlId'] && $row['Controllable'] )
    $showControl = true;
  $row['connKey'] = generateConnKey();
  $Monitor = $monitors[] = new Monitor( $row );
  $monitors_dropdown[$Monitor->Id()] = $Monitor->Name();
  if ( ! isset( $widths[$row['Width']] ) ) {
    $widths[$row['Width']] = $row['Width'];
  }
  if ( ! isset( $heights[$row['Height']] ) ) {
    $heights[$row['Height']] = $row['Height'];
  }
} # end foreach Monitor

xhtmlHeaders(__FILE__, translate('Montage') );
?>
<body>
  <div id="page">
    <?php echo getNavBarHTML() ?>
    <div id="header">
      <div id="headerButtons">
<?php
if ( $showControl ) {
?>
        <a href="#" onclick="createPopup( '?view=control', 'zmControl', 'control' )"><?php echo translate('Control') ?></a>
<?php
}
if ( $showZones ) {
?>
        <a href="<?php echo $_SERVER['PHP_SELF'].'?view=montage&showZones=0'; ?>">Hide Zones</a>
<?php
} else {
?>
        <a href="<?php echo $_SERVER['PHP_SELF'].'?view=montage&showZones=1'; ?>">Show Zones</a>
<?php
}
?>
      </div>
      <div id="headerControl">
        <span id="groupControl"><label><?php echo translate('Group') ?>:</label>
        <?php echo $group_dropdowns; ?>
        </span>
      <span id="monitorControl"><label><?php echo translate('Monitor') ?>:</label>
      <?php echo htmlSelect( 'monitor_id', $monitors_dropdown, $monitor_id, array('onchange'=>'changeMonitor(this);') ); ?>
      </span>
        <span id="widthControl"><label><?php echo translate('Width') ?>:</label><?php echo htmlSelect( 'width', $widths, $options['width'], 'changeSize(this);' ); ?></span>
        <span id="heightControl"><label><?php echo translate('Height') ?>:</label><?php echo htmlSelect( 'height', $heights, $options['height'], 'changeSize(this);' ); ?></span>
        <span id="scaleControl"><label><?php echo translate('Scale') ?>:</label><?php echo htmlSelect( 'scale', $scales, $scale, 'changeScale(this);' ); ?></span> 
        <span id="layoutControl"><label for="layout"><?php echo translate('Layout') ?>:</label><?php echo htmlSelect( 'layout', $layouts, $layout, 'selectLayout(this);' )?></span>
      </div>
    </div>
    <div id="content">
      <div id="monitors">
<?php
foreach ( $monitors as $monitor ) {
  $connkey = $monitor->connKey(); // Minor hack
?>
        <div id="monitorFrame<?php echo $monitor->Id() ?>" class="monitorFrame" title="<?php echo $monitor->Id() . ' ' .$monitor->Name() ?>">
          <div id="monitor<?php echo $monitor->Id() ?>" class="monitor idle">
            <div id="imageFeed<?php echo $monitor->Id() ?>" class="imageFeed" onclick="createPopup( '?view=watch&amp;mid=<?php echo $monitor->Id() ?>', 'zmWatch<?php echo $monitor->Id() ?>', 'watch', <?php echo reScale( $monitor->Width(), $monitor->PopupScale() ); ?>, <?php echo reScale( $monitor->Height(), $monitor->PopupScale() ); ?> );">
            <?php 
              echo getStreamHTML( $monitor, $options );
              if ( $showZones ) { 
                $height = null;
                $width = null;
                if ( $options['width'] ) {
                  $width = $options['width'];
                  if ( ! $options['height'] ) {
                    $scale = (int)( 100 * $options['width'] / $monitor->Width() );
                    $height = reScale( $monitor->Height(), $scale );
                  }
                } else if ( $options['height'] ) {
                  $height =  $options['height'];
                  if ( ! $options['width'] ) {
                    $scale = (int)( 100 * $options['height'] / $monitor->Height() );
                    $width = reScale( $monitor->Width(), $scale );
                  }
                } else if ( $scale ) {
                  $width = reScale( $monitor->Width(), $scale );
                  $height = reScale( $monitor->Height(), $scale );
                } 

                $zones = array();
                foreach( dbFetchAll( 'SELECT * FROM Zones WHERE MonitorId=? ORDER BY Area DESC', NULL, array($monitor->Id()) ) as $row ) {
                  $row['Points'] = coordsToPoints( $row['Coords'] );

                  if ( $scale ) {
                    limitPoints( $row['Points'], 0, 0, $monitor->Width(), $monitor->Height() );
                    scalePoints( $row['Points'], $scale );
                  } else {
                    limitPoints( $row['Points'], 0, 0, 
                        ( $width ? $width-1 : $monitor->Width()-1 ),
                        ( $height ? $height-1 : $monitor->Height()-1 )
                        );
                  }
                  $row['Coords'] = pointsToCoords( $row['Points'] );
                  $row['AreaCoords'] = preg_replace( '/\s+/', ',', $row['Coords'] );
                  $zones[] = $row;
                }

?>

            <svg class="zones" id="zones<?php echo $monitor->Id() ?>" style="position:absolute; top: 0; left: 0; background: none; width: <?php echo $width ?>px; height: <?php echo $height ?>px;">
            <?php
            foreach( array_reverse($zones) as $zone ) {
              ?>
                <polygon points="<?php echo $zone['AreaCoords'] ?>" class="<?php echo $zone['Type']?>" />
                <?php
            } // end foreach zone
?>
  Sorry, your browser does not support inline SVG
  </svg>
<?php } # end if showZones ?>
            </div>
<?php
    if ( !ZM_WEB_COMPACT_MONTAGE ) {
?>
            <div id="monitorState<?php echo $monitor->Id() ?>" class="monitorState idle"><?php echo translate('State') ?>:&nbsp;<span id="stateValue<?php echo $monitor->Id() ?>"></span>&nbsp;-&nbsp;<span id="fpsValue<?php echo $monitor->Id() ?>"></span>&nbsp;fps</div>
<?php
    }
?>
          </div>
        </div>
<?php
}
?>
      </div>
    </div>
  </div>
<?php xhtmlFooter() ?>
