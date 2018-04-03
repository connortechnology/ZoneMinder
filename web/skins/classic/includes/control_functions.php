<?php
//
// ZoneMinder web control function library, $Date$, $Revision$
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

function getControlCommands( $monitor )
{
    $cmds = array();

    $cmds['Wake'] = "wake";
    $cmds['Sleep'] = "sleep";
    $cmds['Reset'] = "reset";

    $cmds['PresetSet'] = "presetSet";
    $cmds['PresetGoto'] = "presetGoto";
    $cmds['PresetHome'] = "presetHome";

    if ( $monitor->CanZoom() )
    {
        if ( $monitor->CanZoomCon() )
            $cmds['ZoomRoot'] = "zoomCon";
        elseif ( $monitor->CanZoomRel() )
            $cmds['ZoomRoot'] = "zoomRel";
        elseif ( $monitor->CanZoomAbs() )
            $cmds['ZoomRoot'] = "zoomAbs";
        $cmds['ZoomTele'] = $cmds['ZoomRoot']."Tele";
        $cmds['ZoomWide'] = $cmds['ZoomRoot']."Wide";
        $cmds['ZoomStop'] = "zoomStop";
        $cmds['ZoomAuto'] = "zoomAuto";
        $cmds['ZoomMan'] = "zoomMan";
    }

    if ( $monitor->CanFocus() )
    {
        if ( $monitor->CanFocusCon() )
            $cmds['FocusRoot'] = "focusCon";
        elseif ( $monitor->CanFocusRel() )
            $cmds['FocusRoot'] = "focusRel";
        elseif ( $monitor->CanFocusAbs() )
            $cmds['FocusRoot'] = "focusAbs";
        $cmds['FocusFar'] = $cmds['FocusRoot']."Far";
        $cmds['FocusNear'] = $cmds['FocusRoot']."Near";
        $cmds['FocusStop'] = "focusStop";
        $cmds['FocusAuto'] = "focusAuto";
        $cmds['FocusMan'] = "focusMan";
    }

    if ( $monitor->CanIris() )
    {
        if ( $monitor->CanIrisCon() )
            $cmds['IrisRoot'] = "irisCon";
        elseif ( $monitor->CanIrisRel() )
            $cmds['IrisRoot'] = "irisRel";
        elseif ( $monitor->CanIrisAbs() )
            $cmds['IrisRoot'] = "irisAbs";
        $cmds['IrisOpen'] = $cmds['IrisRoot']."Open";
        $cmds['IrisClose'] = $cmds['IrisRoot']."Close";
        $cmds['IrisStop'] = "irisStop";
        $cmds['IrisAuto'] = "irisAuto";
        $cmds['IrisMan'] = "irisMan";
    }

    if ( $monitor->CanWhite() )
    {
        if ( $monitor->CanWhiteCon() )
            $cmds['WhiteRoot'] = "whiteCon";
        elseif ( $monitor->CanWhiteRel() )
            $cmds['WhiteRoot'] = "whiteRel";
        elseif ( $monitor->CanWhiteAbs() )
            $cmds['WhiteRoot'] = "whiteAbs";
        $cmds['WhiteIn'] = $cmds['WhiteRoot']."In";
        $cmds['WhiteOut'] = $cmds['WhiteRoot']."Out";
        $cmds['WhiteAuto'] = "whiteAuto";
        $cmds['WhiteMan'] = "whiteMan";
    }

    if ( $monitor->CanGain() )
    {
        if ( $monitor->CanGainCon() )
            $cmds['GainRoot'] = "gainCon";
        elseif ( $monitor->CanGainRel() )
            $cmds['GainRoot'] = "gainRel";
        elseif ( $monitor->CanGainAbs() )
            $cmds['GainRoot'] = "gainAbs";
        $cmds['GainUp'] = $cmds['GainRoot']."Up";
        $cmds['GainDown'] = $cmds['GainRoot']."Down";
        $cmds['GainAuto'] = "gainAuto";
        $cmds['GainMan'] = "gainMan";
    }

    if ( $monitor->CanMove() )
    {
        if ( $monitor->CanMoveCon() )
        {
            $cmds['MoveRoot'] = "moveCon";
            $cmds['Center'] = "moveStop";
        }
        elseif ( $monitor->CanMoveRel() )
        {
            $cmds['MoveRoot'] = "moveRel";
            $cmds['Center'] = $cmds['PresetHome'];
        }
        elseif ( $monitor->CanMoveAbs() )
        {
            $cmds['MoveRoot'] = "moveAbs";
            $cmds['Center'] = $cmds['PresetHome'];
		} else {
			$cmds['MoveRoot'] = '';
        }

        $cmds['MoveUp'] = $cmds['MoveRoot']."Up";
        $cmds['MoveDown'] = $cmds['MoveRoot']."Down";
        $cmds['MoveLeft'] = $cmds['MoveRoot']."Left";
        $cmds['MoveRight'] = $cmds['MoveRoot']."Right";
        $cmds['MoveUpLeft'] = $cmds['MoveRoot']."UpLeft";
        $cmds['MoveUpRight'] = $cmds['MoveRoot']."UpRight";
        $cmds['MoveDownLeft'] = $cmds['MoveRoot']."DownLeft";
        $cmds['MoveDownRight'] = $cmds['MoveRoot']."DownRight";
    }
    return( $cmds );
}

function controlFocus( $monitor, $cmds )
{
    ob_start();
?>
<div class="arrowControl focusControls">
  <div class="arrowLabel"><?php echo translate('Near') ?></div>
  <div class="longArrowBtn upBtn" onclick="controlCmd('<?php echo $cmds['FocusNear'] ?>',event,0,-1)"></div>
  <div class="arrowCenter"<?php if ( $monitor->CanFocusCon() ) { ?> onclick="controlCmd('<?php echo $cmds['FocusStop'] ?>')"<?php } ?>><?php echo translate('Focus') ?></div>
  <div class="longArrowBtn downBtn" onclick="controlCmd('<?php echo $cmds['FocusFar'] ?>',event,0,1)"></div>
  <div class="arrowLabel"><?php echo translate('Far') ?></div>
<?php
    if ( $monitor->CanAutoFocus() )
    {
?>
  <input type="button" class="ptzTextBtn" value="<?php echo translate('Auto') ?>" onclick="controlCmd('<?php echo $cmds['FocusAuto'] ?>')"/>
  <input type="button" class="ptzTextBtn" value="<?php echo translate('Man') ?>" onclick="controlCmd('<?php echo $cmds['FocusMan'] ?>')"/>
<?php
    }
?>
</div>
<?php
    return( ob_get_clean() );
}

function controlZoom( $monitor, $cmds )
{
    global $SLANG;

    ob_start();
?>
<div class="arrowControl zoomControls">
  <div class="arrowLabel"><?php echo translate('Tele') ?></div>
  <div class="longArrowBtn upBtn" onclick="controlCmd('<?php echo $cmds['ZoomTele'] ?>',event,0,-1)"></div>
  <div class="arrowCenter"<?php if ( $monitor->CanZoomCon() ) { ?> onclick="controlCmd('<?php echo $cmds['ZoomStop'] ?>')"<?php } ?>><?php echo translate('Zoom') ?></div>
  <div class="longArrowBtn downBtn" onclick="controlCmd('<?php echo $cmds['ZoomWide'] ?>',event,0,1)"></div>
  <div class="arrowLabel"><?php echo translate('Wide') ?></div>
<?php
    if ( $monitor->CanAutoZoom() )
    {
?>
  <input type="button" class="ptzTextBtn" value="<?php echo translate('Auto') ?>" onclick="controlCmd('<?php echo $cmds['ZoomAuto'] ?>')"/>
  <input type="button" class="ptzTextBtn" value="<?php echo translate('Man') ?>" onclick="controlCmd('<?php echo $cmds['ZoomMan'] ?>')"/>
<?php
    }
?>
</div>
<?php
    return( ob_get_clean() );
}

function controlIris( $monitor, $cmds )
{
    global $SLANG;

    ob_start();
?>
<div class="arrowControl irisControls">
  <div class="arrowLabel"><?php echo translate('Open') ?></div>
  <div class="longArrowBtn upBtn" onclick="controlCmd('<?php echo $cmds['IrisOpen'] ?>',event,0,-1)"></div>
  <div class="arrowCenter"<?php if ( $monitor->CanIrisCon() ) { ?> onclick="controlCmd('<?php echo $cmds['IrisStop'] ?>')"<?php } ?>><?php echo translate('Iris') ?></div>
  <div class="longArrowBtn downBtn" onclick="controlCmd('<?php echo $cmds['IrisClose'] ?>',event,0,1)"></div>
  <div class="arrowLabel"><?php echo translate('Close') ?></div>
<?php
    if ( $monitor->CanAutoIris() )
    {
?>
  <input type="button" class="ptzTextBtn" value="<?php echo translate('Auto') ?>" onclick="controlCmd('<?php echo $cmds['IrisAuto'] ?>')"/>
  <input type="button" class="ptzTextBtn" value="<?php echo translate('Man') ?>" onclick="controlCmd('<?php echo $cmds['IrisMan'] ?>')"/>
<?php
    }
?>
</div>
<?php
    return( ob_get_clean() );
}

function controlWhite( $monitor, $cmds )
{
    global $SLANG;

    ob_start();
?>
<div class="arrowControl whiteControls">
  <div class="arrowLabel"><?php echo translate('In') ?></div>
  <div class="longArrowBtn upBtn" onclick="controlCmd('<?php echo $cmds['WhiteIn'] ?>',event,0,-1)"></div>
  <div class="arrowCenter"<?php if ( $monitor->CanWhiteCon() ) { ?> onclick="controlCmd('<?php echo $cmds['WhiteStop'] ?>')"<?php } ?>><?php echo translate('White') ?></div>
  <div class="longArrowBtn downBtn" onclick="controlCmd('<?php echo $cmds['WhiteOut'] ?>',event,0,1)"></div>
  <div class="arrowLabel"><?php echo translate('Out') ?></div>
<?php
    if ( $monitor->CanAutoWhite() )
    {
?>
  <input type="button" class="ptzTextBtn" value="<?php echo translate('Auto') ?>" onclick="controlCmd('<?php echo $cmds['WhiteAuto'] ?>')"/>
  <input type="button" class="ptzTextBtn" value="<?php echo translate('Man') ?>" onclick="controlCmd('<?php echo $cmds['WhiteMan'] ?>')"/>
<?php
    }
?>
</div>
<?php
    return( ob_get_clean() );
}

function controlPanTilt( $monitor, $cmds )
{
    global $SLANG;

    ob_start();
?>
<div class="pantiltControls">
  
<?php
    $hasPan = $monitor->CanPan();
    $hasTilt = $monitor->CanTilt();
    $hasDiag = ($hasPan && $hasTilt && $monitor->CanMoveDiag());

    // some of these may be null, which needs to be correct
    // else JS call will fail with syntax error
    $hasPan =  $hasPan?:'0';
    $hasTilt = $hasTilt?:'0';
    $hasDiag = $hasDiag?:'0';
  
    echo '<script>ptzmenu('.$hasPan.','.$hasTilt.','.$hasDiag.','.json_encode($cmds).');</script>';
?>
</div>
<?php
    return( ob_get_clean() );
}

function controlPresets( $monitor, $cmds )
{
    global $SLANG;

    define( "MAX_PRESETS", "12" );

    $sql = 'select * from ControlPresets where MonitorId = ?';
    $labels = array();
    foreach( dbFetchAll( $sql, NULL, array( $monitor->Id() ) ) as $row )
    {
        $labels[$row['Preset']] = $row['Label'];
    }

    $presetBreak = (int)(($monitor->NumPresets()+1)/((int)(($monitor->NumPresets()-1)/MAX_PRESETS)+1));

    ob_start();
?>
<div class="presetControls">

<?php
    for ( $i = 1; $i <= $monitor->NumPresets(); $i++ )
    {
?>
        <input type="button" class="ptzNumBtn" title="<?php echo isset($labels[$i])?$labels[$i]:"" ?>" value="<?php echo $i ?>" onclick="controlCmd('<?php echo $cmds['PresetGoto'] ?><?php echo $i ?>');"/>
<?php
        if ( $i && (($i%$presetBreak) == 0) )
        {
?>
<?php
        }
    }
?>


<?php
    if ( $monitor->HasHomePreset() )
    {
?>
    <input type="button" class="ptzTextBtn" value="<?php echo translate('Home') ?>" onclick="controlCmd('<?php echo $cmds['PresetHome'] ?>');"/>
<?php
    }
    if ( canEdit( 'Monitors') && $monitor->CanSetPresets() )
    {
?>
    <input type="button" class="ptzTextBtn" value="<?php echo translate('Set') ?>" onclick="createPopup( '?view=controlpreset&amp;mid=<?php echo $monitor->Id() ?>', 'zmPreset', 'preset' );"/>
<?php
    }
?>

</div>
<?php
    return( ob_get_clean() );
}

function controlPower( $monitor, $cmds )
{
    global $SLANG;

    ob_start();
?>
<div class="powerControls">
<div class="powerLabel"><?php echo translate('Control') ?></div>
  <div>
<?php
    if ( $monitor->CanWake() )
    {
?>
        <button type="button" class="ptzTextBtn" onclick="controlCmd('<?php echo $cmds['Wake'] ?>')"/>
            <span class="glyphicon glyphicon-eye-open"></span>&nbsp;<?php echo translate('Wake') ?>
        </button>
 
<?php
    }
    if ( $monitor->CanSleep() )
    {
?>
        <button type="button" class="ptzTextBtn" onclick="controlCmd('<?php echo $cmds['Sleep'] ?>')"/>
            <span class="glyphicon glyphicon-eye-close"></span>&nbsp;<?php echo translate('Sleep') ?>
        </button>
    
<?php
    }
    if ( $monitor->CanReset() )
    {
?>
        <button type="button" class="ptzTextBtn" onclick="controlCmd('<?php echo $cmds['Reset'] ?>')"/>
            <span class="glyphicon glyphicon-refresh"></span>&nbsp;<?php echo translate('Reset') ?>
        </button>

<?php
    }
?>
</div>
</div>
<?php
    return( ob_get_clean() );
}

function ptzControls( $monitor )
{
    $cmds = getControlCommands( $monitor );
    ob_start();
?>
    
    <div class="controlsPanel">  

   
			<?php
			
        		if ( $monitor->HasPresets() )
                        echo controlPresets( $monitor, $cmds );
            ?>
	

	<table class="table" >
		<tr class="row">
       
            <?php 
                if ( $monitor->CanFocus() )
                    echo "<td class='col'>".controlFocus( $monitor, $cmds )."</td>";
                if ( $monitor->CanZoom() )
                    echo  "<td class='col'>".controlZoom( $monitor, $cmds )."</td>";
                if ( $monitor->CanIris() )
                    echo "<td class='col'>".controlIris( $monitor, $cmds )."</td>";
                if ( $monitor->CanWhite() )
                    echo "<td class='col'>".controlWhite( $monitor, $cmds )."</td>";
            ?>
            
            <div class="pantiltPanel">
				<?php
					if ( $monitor->CanMove() ) {
                       
      					echo "<td class='col'>"."<div class='pantiltLabel'>".translate('PanTilt')."</div>"."<div id='ptzNavWheel'></div>"."</td>";
                        echo "<td class='col'>".controlPanTilt( $monitor, $cmds )."</td>";			
                    }
                    if ( $monitor->CanWake() || $monitor->CanSleep() || $monitor->CanReset() )
        		    echo "<td class='col'>".controlPower( $monitor, $cmds )."</td>";
				?>
             </div>
		</tr>
		

	</table>
    </div>

<?php
    return( ob_get_clean() );
}
?>
