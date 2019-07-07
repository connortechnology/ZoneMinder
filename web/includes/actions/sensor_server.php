<?php
//
// ZoneMinder web action file
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

// System edit actions
if ( ! canEdit('System') ) {
  ZM\Warning('Need System permissions to add servers');
  return;
}

require_once('includes/Sensor_Server.php');
require_once('includes/Sensor_Server_Type.php');
require_once('includes/Sensor.php');

if ( !empty($_REQUEST['id']) ) {
  $Server = ZM\Sensor_Server::find_one(array('Id'=>$_REQUEST['id']));
  if ( !$Server ) {
    ZM\Error('Server not found');
    return;
  }
}

if ( $action == 'Save' ) {

  if ( !$Server )
    $Server = new ZM\Sensor_Server();
 
  $changes = $Server->changes($_REQUEST['newServer']);

  if ( count($changes) ) {
    if ( $Server->Id() ) {
      $Server->save($changes);
      $view = 'none';
    } else {
      $Server->set($changes);
      if ( $Server->TypeId() ) {
        $ServerType = ZM\Sensor_Server_Type::find_one(array('Id'=>$Server->TypeId()));
        if ( $ServerType->Chains() and ! $Server->Chains() )
          $Server->Chains($ServerType->Chains());
        $Server->save();
        if ( $ServerType->MaxSensors() ) {
          if ( $Server->Chains() ) {
            $chunks = array_chunk(preg_split('/(:|,)/', $Server->Chains()), 2);
            $chains = array_combine(array_column($chunks, 0), array_column($chunks,1));
          } else {
            $chains = array(null=>'');
          }
          foreach ( $chains as $chain_id=>$chain_name ) {
            for ( $sensor_id = 1; $sensor_id < $ServerType->MaxSensors(); $sensor_id += 1 ) {
              $Sensor = new ZM\Sensor();
              $Sensor->save(array('SensorServerId'=>$Server->Id(), 'Chain'=>$chain_id, 'SensorId'=>$sensor_id));
            }
          } # end foreach Chain
        }
      } else {
        ZM\Error("No TypeId" . $Server->TypeId());
        $Server->save();
      }
      $_REQUEST['id'] = $Server->Id();
    }
    if ( $Server->Enabled() ) {
      daemonControl('restart', 'zmsensor_monitor.pl --daemon --server_id='.$Server->Id());
    } else {
      daemonControl('stop', 'zmsensor_monitor.pl --daemon --server_id='.$Server->Id());
    }
    $refreshParent = true;
  }
} else if ( $action == 'add_more_sensors' ) {
  $ServerType = ZM\Sensor_Server_Type::find_one(array('Id'=>$Server->TypeId()));
  if ( $ServerType->MaxSensors() ) {
    if ( $Server->Chains() ) {
      $chunks = array_chunk(preg_split('/(:|,)/', $Server->Chains()), 2);
      $chains = array_combine(array_column($chunks, 0), array_column($chunks,1));
    } else {
      $chains = array(null=>'');
    }
    foreach ( $chains as $chain_id=>$chain_name ) {
      if ( isset($_REQUEST['chain_id']) and ($chain_id != $_REQUEST['chain_id']) )
        continue;

      $max_sensor = ZM\Sensor::find_one(array('SensorServerId'=>$Server->Id(), 'Chain'=>$chain_id), array('order'=>'Id DESC'));
      $start_id = $max_sensor ? intval($max_sensor->SensorId()) + 1 : 1;
      $end_id = $start_id + $ServerType->MaxSensors();
      ZM\Logger::Debug("From $start_id top $end_id");
      for (
        $sensor_id = $start_id;
        $sensor_id < $end_id;
        $sensor_id += 1
        ) {
        $Sensor = new ZM\Sensor();
        $Sensor->save(array('SensorServerId'=>$Server->Id(), 'Chain'=>$chain_id, 'SensorId'=>$sensor_id));
      }
    } # end foreach Chain
  } # end if has Max Sensors
  $redirect = $Server->link_to();
} else {
  ZM\Error("Unknown action $action in saving Sensor_Server");
}
?>
