<?php
//
// ZoneMinder web sensor_action view file
// Copyright (C) 2019 ZoneMinder LLC
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

if ( !canEdit('System') ) {
  $view = 'error';
  return;
}
$canEdit = canEdit('System');

require_once('includes/Sensor_Action.php');
require_once('includes/Sensor_Action_Type.php');
require_once('includes/Sensor.php');
require_once('includes/Monitor.php');

$Action = new ZM\Sensor_Action($_REQUEST['id']);
if ( $_REQUEST['id'] and ! $Action->Id() ) {
  $view = 'error';
  return;
}
if ( isset($_REQUEST['newAction']) ) {
  $Action->set($_REQUEST['newAction']);
}

$focusWindow = true;

xhtmlHeaders(__FILE__, translate('SensorAction').' - '.$Action->Name());
?>
<body>
  <div id="page">
    <div id="header">
      <h2><?php echo translate('SensorAction').' - '.$Action->Name() ?></h2>
    </div>
    <div id="content">
      <form name="contentForm" method="post" action="?" class="validateFormOnSubmit">
        <input type="hidden" name="view" value="<?php echo $view ?>"/>
        <input type="hidden" name="action" value="sensor_action"/>
        <input type="hidden" name="id" value="<?php echo validHtmlStr($_REQUEST['id']) ?>"/>
        <table id="contentTable" class="major">
          <tbody>
            <tr>
              <th scope="row"><?php echo translate('Sensor') ?></th>
              <td>
<?php
$Sensors = array(''=>'Unknown')+ZM\Sensor::Objects_Indexed_By_Id();
echo htmlSelect('newAction[SensorId]', $Sensors, $Action->SensorId(),
    array(
      'class'=>'chosen',
      'data-placeholder'=>'Unknown',
      'data-on-change-this'=>'updateButtons'
    ) );
?>
            </tr>
            <tr>
              <th scope="row"><?php echo translate('Name') ?></th>
              <td><input type="text" name="newAction[Name]" value="<?php echo $Action->Name() ?>"/></td>
            </tr>
<tr>
              <th scope="row"><?php echo translate('Type') ?></th>
              <td>
<?php
$Types = array(''=>'Unknown')+ZM\Sensor_Action_Type::Objects_Indexed_By_Id();
echo htmlSelect('newAction[TypeId]', $Types, $Action->TypeId(),
    array(
      'class'=>'chosen',
      'data-placeholder'=>'Unknown',
      'data-on-change-this'=>'updateButtons'
    ) );
?>
            </tr>

            <tr>
              <th scope="row"><?php echo translate('MinValue') ?></th>
              <td><input type="number" name="newAction[MinValue]" value="<?php echo $Action->MinValue() ?>" data-on-input-this="updateButtons"/></td>
            </tr>
            <tr>
              <th scope="row"><?php echo translate('MaxValue') ?></th>
              <td><input type="number" name="newAction[MaxValue]" value="<?php echo $Action->MaxValue() ?>" data-on-input-this="updateButtons"/></td>
            </tr>
            <tr>
              <th scope="row"><?php echo translate('Monitor') ?></th>
              <td>
<?php
$Monitors = array(''=>'Unknown')+ZM\Monitor::Objects_Indexed_By_Id();
echo htmlSelect('newAction[MonitorId]', $Monitors, $Action->MonitorId(),
  array( 'class'=>'chosen',
      'data-placeholder'=>'Unknown',
      'data-on-change-this'=>'updateButtons'
) );
?>
</td>
            </tr>
            <tr>
              <th scope="row"><?php echo translate('Action') ?></th>
              <td><input type="text" name="newAction[Action]" value="<?php echo $Action->Action() ?>" data-on-input-this="updateButtons"/></td>
            </tr>
          </tbody>
        </table>
        <div id="contentButtons">
          <button type="submit" name="action" value="Save" id="SaveButton"><?php echo translate('Save') ?></button>
          <button type="submit" name="action" value="Delete" <?php echo $Action->Id() ? '' : 'disabled'?>><?php echo translate('Delete') ?></button>
          <button type="button" data-on-click="closeWindow"><?php echo translate('Cancel') ?></button>
        </div>
      </form>
    </div>
  </div>
</body>
</html>
