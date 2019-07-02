<?php
namespace ZM;
require_once('database.php');
require_once('Sensor.php');
require_once('Object.php');

class Sensor_Server extends ZM_Object {
  protected static $table = 'Sensor_Servers';
  protected $defaults = array(
    'Id'                   => null,
    'TypeId'               => null,
    'Name'                 => '',
    'Url'                  => '',
    'PollFrequency'        => 3,
    'Chains'               => null,
  );

  public function Sensors($new=-1) {
    if ( $new != -1 ) {
      $this->{'Sensors'} = $new;
    }

    if ( !array_key_exists('Sensors', $this) ) {
      $this->{'Sensors'} = Sensor::find(array('SensorServerId'=>$this->Id()));
    }
    return $this->{'Sensors'};
  } # end public function Sensors()

  public static function find($parameters = array(), $options = array() ) {
    return ZM_Object::_find(get_class(), $parameters, $options);
  }

  public static function find_one( $parameters = array() ) {
    return ZM_Object::_find_one(get_class(), $parameters);
  }

  static function Objects_Indexed_By_Id() {
    $Objects = array();
    foreach ( ZM_Object::_find(get_class(), null, array('order'=>'lower(Name)')) as $Object ) {
      $Objects[$Object->Id()] = $Object;
    }
    return $Objects;
  }

  public function Chains_array() {
    if ( $this->Chains() ) {
      $chunks = array_chunk(preg_split('/(:|,)/', $this->Chains()), 2);
      $chains = array_combine(array_column($chunks, 0), array_column($chunks, 1));
    } else {
      $chains = array(null=>'');
    }
    return $chains;
  }

} # end class Sensor_Server
?>
