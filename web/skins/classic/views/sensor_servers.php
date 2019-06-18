<?php
//
// ZoneMinder web options view file, $Date$, $Revision$
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

if ( !canView('System') ) {
  $view = 'error';
  return;
}

$canEdit = canEdit('System');
require_once('includes/Sensor_Server.php');

?>
  <form name="serversForm" method="post" action="?">
    <input type="hidden" name="view" value="<?php echo $view ?>"/>
    <input type="hidden" name="tab" value="<?php echo $tab ?>"/>
    <input type="hidden" name="action" value="delete"/>
    <input type="hidden" name="object" value="server"/>
    <table id="contentTable" class="table table-striped">
      <thead class="thead-highlight">
        <tr>
          <th class="colName"><?php echo translate('Name') ?></th>
          <th class="colUrl"><?php echo translate('Url') ?></th>
          <th class="colPollFrequency"><?php echo translate('Poll Frequency') ?></th>
          <th class="colSensorCount"><?php echo translate('Sensors') ?></th>
          <th class="colMark"><?php echo translate('Mark') ?></th>
        </tr>
      </thead>
      <tbody>
<?php
$monitor_counts = dbFetchAssoc('SELECT Id,(SELECT COUNT(Id) FROM Sensors WHERE SensorServerId=Sensor_Servers.Id) AS SensorCount FROM Sensor_Servers', 'Id', 'SensorCount');
foreach ( ZM\Sensor_Server::find() as $Server ) {
?>
        <tr>
          <td class="colName"><?php echo makePopupLink('?view=sensor_server&amp;id='.$Server->Id(), 'zmSensorServer', 'sensor_server', validHtmlStr($Server->Name()), $canEdit) ?></td>
          <td class="colUrl"><?php echo makePopupLink('?view=sensor_server&amp;id='.$Server->Id(), 'zmSensorServer', 'sensor_server', validHtmlStr($Server->Url()), $canEdit) ?></td>
          <td class="colPollFrequency"><?php echo makePopupLink('?view=sensor_server&amp;id='.$Server->Id(), 'zmSensorServer', 'sensor_server', validHtmlStr($Server->PollFrequency()), $canEdit) ?></td>
          <td class="colMonitorCount">
              <?php echo makePopupLink('?view=sensor_server&amp;id='.$Server->Id(), 'zmSensorServer', 'sensor_server', validHtmlStr($monitor_counts[$Server->Id()]), $canEdit) ?>
          </td>
          <td class="colMark"><input type="checkbox" name="markIds[]" value="<?php echo $Server->Id() ?>" data-on-click-this="configureDeleteButton"<?php if ( !$canEdit ) { ?> disabled="disabled"<?php } ?>/></td>
  </tr>
<?php } #end foreach Server ?>
      </tbody>
    </table>
    <div id="contentButtons">
    <?php echo makePopupButton('?view=server&id=0', 'zmServer', 'server', translate('AddNewServer'), canEdit('System')); ?>
      <button type="submit" class="btn-danger" name="deleteBtn" value="Delete" disabled="disabled"><?php echo translate('Delete') ?></button>
    </div>
  </form>
