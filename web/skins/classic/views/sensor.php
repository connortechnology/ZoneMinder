<?php
//
// ZoneMinder web sensor view file
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
require_once('includes/Sensor_Server.php');
require_once('includes/Sensor.php');
require_once('includes/Monitor.php');

$Sensor = new ZM\Sensor($_REQUEST['id']);
if ( $_REQUEST['id'] and ! $Sensor->Id() ) {
  $view = 'error';
  return;
}
if ( isset($_REQUEST['newSensor']) ) {
  $Sensor->set($_REQUEST['newSensor']);
}

$focusWindow = true;

xhtmlHeaders(__FILE__, translate('Sensor').' - '.$Sensor->Name());
?>
<body>
  <div id="page">
    <div id="header">
      <h2><?php echo translate('Sensor').' - '.$Sensor->Name() ?></h2>
    </div>
    <div id="content">
      <form name="contentForm" method="post" action="?" class="validateFormOnSubmit">
        <input type="hidden" name="view" value="<?php echo $view ?>"/>
        <input type="hidden" name="action" value="sensor"/>
        <input type="hidden" name="id" value="<?php echo validHtmlStr($_REQUEST['id']) ?>"/>
        <table id="contentTable" class="major">
          <tbody>
            <tr>
              <th scope="row"><?php echo translate('SensorServer') ?></th>
              <td>
<?php
$SensorServers = array(''=>'Unknown')+ZM\Sensor_Server::Objects_Indexed_By_Id();
echo htmlSelect('newSensor[SensorServerId]', $SensorServers, $Sensor->SensorServerId(),
    array(
      'class'=>'chosen',
      'data-placeholder'=>'Unknown',
      'data-on-change-this'=>'updateButtons'
    ) );
?>
            </tr>
            <tr>
              <th scope="row"><?php echo translate('Name') ?></th>
              <td><input type="text" name="newSensor[Name]" value="<?php echo $Sensor->Name() ?>"/></td>
            </tr>
            <tr>
              <th scope="row"><?php echo translate('SensorChain') ?></th>
              <td><input type="text" name="newSensor[Chain]" value="<?php echo $Sensor->Chain() ?>"/></td>
            </tr>
            <tr>
              <th scope="row"><?php echo translate('SensorId') ?></th>
              <td><input type="text" name="newSensor[SensorId]" value="<?php echo $Sensor->SensorId() ?>"/></td>
            </tr>
            <tr>
              <th scope="row"><?php echo translate('MinValue') ?></th>
              <td><input type="number" name="newSensor[MinValue]" value="<?php echo $Sensor->MinValue() ?>" data-on-input-this="updateButtons"/>
Values detected below this will generate a warning. Leave blank for no limit.
</td>
            </tr>
            <tr>
              <th scope="row"><?php echo translate('MaxValue') ?></th>
              <td><input type="number" name="newSensor[MaxValue]" value="<?php echo $Sensor->MaxValue() ?>" data-on-input-this="updateButtons"/>
Values detected above this will generate a warning. Leave blank for no limit.
</td>
            </tr>
          </tbody>
        </table>

<div class="Actions">
      <fieldset><legend>Actions</legend>
          <table>
            <thead>
              <tr>
                <th class="Name"><?php echo translate('Name') ?></th>
                <th class="Type"><?php echo translate('Type') ?></th>
                <th class="MinValue"><?php echo translate('MinValue') ?></th>
                <th class="MaxValue"><?php echo translate('MaxValue') ?></th>
                <th class="Monitor"><?php echo translate('Monitor') ?></th>
                <th class="Monitor"><?php echo translate('Actions') ?></th>
                <th class="buttons"><?php echo makePopupButton((new ZM\Sensor_Action())->link_to().'&amp;newSensor[SensorId]='.$Sensor->Id(),'zmSensorActionNew', 'sensor_action', '+', $canEdit ) ?></th>
              </tr>
            </thead>
            <tbody>
<?php
foreach ( ZM\Sensor_Action::find(array('SensorId'=>$Sensor->Id())) as $Action ) {
  echo sprintf('
    <tr>
      <td class="Name">%3$s</td>
      <td class="Type">%4$s</td>
      <td class="MinValue">%5$s</td>
      <td class="MaxValue">%6$s</td>
      <td class="Monitor">%7$s</td>
      <td class="Actions">%8$s</td>
      <td class="buttons">%9$s</td>
    </tr>',
    $Action->Id(), $Action->SensorId(), $Action->Name(), $Action->Type(), $Action->MinValue(), $Action->MaxValue(),
    $Action->Monitor()->Name(),
    $Action->Action(),
    makePopupButton($Action->link_to(), 'zmAction'.$Action->Id(), 'sensor_action', 'Edit', $canEdit)
  );
} # end foreach

?>
            </tbody>
          </table>
        </fieldset>
</div>


        <div id="contentButtons">
          <button type="submit" name="action" value="Save" id="SaveButton"><?php echo translate('Save') ?></button>
          <button type="submit" name="action" value="Delete" <?php echo $Sensor->Id() ? '' : 'disabled'?>><?php echo translate('Delete') ?></button>
          <button type="button" data-on-click="closeWindow"><?php echo translate('Cancel') ?></button>
        </div>
      </form>
    </div>
  </div>
</body>
</html>
