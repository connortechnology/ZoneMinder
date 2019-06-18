<?php
//
// ZoneMinder web sensor_server view file
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

$canEdit = canEdit('System');
if ( !$canEdit ) {
  $view = 'error';
  return;
}

require_once('includes/Sensor_Server.php');
$Server = new ZM\Sensor_Server($_REQUEST['id']);
if ( $_REQUEST['id'] and ! $Server->Id() ) {
  $view = 'error';
  return;
}

$focusWindow = true;

xhtmlHeaders(__FILE__, translate('SensorServer').' - '.$Server->Name());
?>
<body>
  <div id="page">
    <div id="header">
      <h2><?php echo translate('SensorServer').' - '.$Server->Name() ?></h2>
    </div>
    <div id="content">
      <form name="contentForm" method="post" action="?" class="validateFormOnSubmit">
        <input type="hidden" name="view" value="<?php echo $view ?>"/>
        <input type="hidden" name="action" value="sensor_server"/>
        <input type="hidden" name="id" value="<?php echo validHtmlStr($_REQUEST['id']) ?>"/>
        <table id="contentTable" class="major">
          <tbody>
            <tr>
              <th scope="row"><?php echo translate('Name') ?></th>
              <td><input type="text" name="newServer[Name]" value="<?php echo $Server->Name() ?>"/></td>
            </tr>
            <tr>
              <th scope="row"><?php echo translate('Url') ?></th>
              <td><input type="url" name="newServer[Url]" value="<?php echo $Server->Url() ?>"/></td>
            </tr>
            <tr>
              <th scope="row"><?php echo translate('PollFrequency') ?></th>
              <td><input type="number" name="newServer[PollFrequency]" value="<?php echo $Server->PollFrequency() ?>"/><?php echo translate('Seconds');?></td>
            </tr>
          </tbody>
        </table>
<fieldset><legend>Sensors</legend>
        <table>
          <thead>
            <tr>
              <th class="SensorId">Id</th>
              <th class="SensorName"><?php echo translate('Name') ?></th>
              <th class="SensorActions"><?php echo translate('Actions') ?></th>
            </tr>
          </thead>
          <tbody>
<?php

$action_link = function($Action){
  global $canEdit;
  return makePopupLink(
    $Action->link_to(),
    'zmAction'.$Action->Id(),
    'sensor_action',
    $Action->to_string(),
    $canEdit );
};
foreach ( $Server->Sensors() as $Sensor ) {
  echo sprintf('
    <tr>
      <td class="SensorId"><input type="text" name="Sensor%1$dd[SensorId]" value="%2$s"/></td>
      <td class="SensorName"><input type="text" name="Sensor%1$dd[SensorName]" value="%3$s"/></td>
      <td class="SensorActions">%4$s</td>
      <td class="buttons">%5$s</td>
    </tr>', $Sensor->Id(), $Sensor->SensorId(), $Sensor->Name(),
    implode('<br/>', array_map($action_link, $Sensor->Actions() )),
    makePopupButton((new ZM\Sensor_Action())->link_to().'&amp;newAction[SensorId]='.$Sensor->Id(),
    'zmActionNew',
    'sensor_action',
    '+',
    $canEdit )
  );
}
?>
          </tbody>
        </table>
</fieldset>
        <div id="contentButtons">
          <button type="submit" name="action" value="Save" ><?php echo translate('Save') ?></button>
          <button type="button" data-on-click="closeWindow"><?php echo translate('Cancel') ?></button>
        </div>
      </form>
    </div>
  </div>
</body>
</html>
