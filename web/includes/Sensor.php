<?php
namespace ZM;
require_once('database.php');
require_once('Sensor_Server.php');
require_once('Sensor_Action.php');

class Sensor extends ZM_Object {
  protected static $table = 'Sensors';
  protected $defaults = array(
    'Id'                   => null,
    'SensorId'             => '',
    'SensorServerId'       => null,
    'Name'                 => '',
    'Chain'                => null,
    'MaxValue'             => null,
    'MinValue'             => null,
  );

  public static function find($parameters = array(), $options = array() ) {
    return ZM_Object::_find(get_class(), $parameters, $options);
  }

  public static function find_one( $parameters = array(), $options = array() ) {
    return ZM_Object::_find_one(get_class(), $parameters, $options);
  }

  public function Actions($new=-1) {
    if ( $new != -1 ) {
      $this->{'Actions'} = $new;
    }

    if ( !(array_key_exists('Actions', $this) and $this->{'Actions'}) ) {
      $sql = 'SELECT * FROM `Sensor_Actions` WHERE
	      `Chain`=? AND
        ((`MinSensorId` IS NULL) OR (`MinSensorId` <= ?))
        AND
        ((`MaxSensorId` IS NULL) OR (`MaxSensorId` >= ?))';
      $this->{'Actions'} = array_map(
        function($row){ return new Sensor_Action($row); },
        dbFetchAll($sql, null, array($this->{'Chain'}, $this->{'Id'}, $this->{'Id'}))
        );
    }
    return $this->{'Actions'};
  } # end public function Actions

  static function Objects_Indexed_By_Id( $Sensors = null ) {
    if ( !$Sensors )
      $Sensors = Sensor::find(null, array('order'=>'lower(Name)'));

    $Objects = array();
    foreach ( $Sensors as $Object ) {
      $Objects[$Object->Id()] = 'Chain ' . $Object->Chain() . ' ' . $Object->SensorId();
    }
    return $Objects;
  }
  public function link_to() {
    return '?view=sensor&id='.$this->{'Id'};
  }
  public function Server() {
    return Sensor_Server::find_one(array('Id'=>$this->SensorServerId()));
  }

} # end class Sensor
?>
