<?php
namespace ZM;
require_once('database.php');
require_once('Monitor.php');
require_once('Object.php');
require_once('Sensor_Action_Type.php');
require_once('Sensor.php');

class Sensor_Action extends ZM_Object {
  protected static $table = 'Sensor_Actions';
  protected $defaults = array(
    'Id'          => null,
    'Name'        => '',
    'Chain'                => null,
    'MinSensorId' => null,
    'MaxSensorId' => null,
    'MonitorId'   => null,
    'TypeId'      => null,
    'MinValue'    => null,
    'MaxValue'    => null,
    'Action'      => '',
  );

  public static function find($parameters = array(), $options = array() ) {
    return ZM_Object::_find(get_class(), $parameters, $options);
  }

  public static function find_one( $parameters = array(), $options = array() ) {
    return ZM_Object::_find_one(get_class(), $parameters, $options);
  }

  public function Monitor($new=-1) {
    if ( $new != -1 )
      $this->{'Monitor'} = $new;

    if ( !(array_key_exists('Monitor', $this) and $this->{'Monitor'} ) ) {
      $this->{'Monitor'} = Monitor::find_one(array('Id'=>$this->MonitorId()));
      if ( ! $this->{'Monitor'} )
        $this->{'Monitor'} = new Monitor();
    }
    return $this->{'Monitor'};
  }

  public function Sensors($new=-1) {
    if ( $new != -1 )
      $this->{'Sensors'} = $new;

    if ( !( array_key_exists('Sensors', $this) and $this->{'Sensors'} ) ) {
      $sql = 'SELECT * FROM Sensors WHERE `Id` >= ? AND `Id` <= ?';
      $this->{'Sensors'} = array_map(
        function($row){ return new Sensor($row); },
          dbFetchAll($sql, null, array($this->{'MinSensorId'}, $this->{'MaxSensorId'}))
        );
    }
    return $this->{'Sensors'};
  }

  public function link_to() {
    return '?view=sensor_action&id='.$this->{'Id'};
  }

  public function to_string() {
    $string = '';
    $string .= $this->Name(). ' ';
    $string .= $this->Monitor()->Name(). ' ';
    if ( ! ( $this->{'MinValue'} and  $this->{'MaxValue'} ) ) {
      $string .= 'Any Value';
    } else {
      if ( $this->{'MinValue'} )
        $string .= $this->{'MinValue'} .' <= ';
      $string .= 'Value';
      if ( $this->{'MaxValue'} )
        $string .= ' >= ' .$this->{'MaxValue'};
    }
    $string .= $this->Action();
    return $string;
  } # end public function to_string

  public function Type() {
	  return Sensor_Action_Type::find_one(array('Id'=>$this->{'TypeId'}));
  }
} # end class Sensor Action
?>
