<?php
namespace ZM;
require_once('database.php');
require_once('Monitor.php');
require_once('Object.php');
require_once('Sensor_Action_Type.php');
require_once('Sensor.php');

$sensor_action_cache = array();

class Sensor_Action extends ZM_Object {
  protected static $table = 'Sensor_Actions';
  protected $defaults = array(
    'Id'          => null,
    'Name'        =>  '',
    'SensorId'    => null,
    'MonitorId'   => null,
    'TypeId'      => null,
    'MinValue'    =>  null,
    'MaxValue'    =>  null,
    'Action'      =>  '',
  );

  public static function find($parameters = array(), $options = array() ) {
    return ZM_Object::_find(get_class(), $parameters, $options);
  }

  public static function find_one( $parameters = array() ) {
    return ZM_Object::_find_one(get_class(), $parameters);
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

  public function Sensor($new=-1) {
    if ( $new != -1 )
      $this->{'Sensor'} = $new;

    if ( !(array_key_exists('Sensor', $this) and $this->{'Sensor'} ) ) {
      $this->{'Sensor'} = Sensor::find_one(array('Id'=>$this->SensorId()));
      if ( ! $this->{'Sensor'} )
        $this->{'Sensor'} = new Sensor();
    }
    return $this->{'Sensor'};
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
    Error($string);
    return $string;
  } # end public function to_string

  public function Type() {
	  return Sensor_Action_Type::find_one(array('Id'=>$this->{'TypeId'}));
  }
} # end class Sensor Action
?>
