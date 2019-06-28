<?php
namespace ZM;
require_once('database.php');
require_once('Object.php');

class Sensor_Server_Type extends ZM_Object {
  protected static $table = 'Sensor_Server_Types';
  private $defaults = array(
    'Id'         => null,
    'Name'       => '',
    'Chains'     => null,
    'MaxSensors' =>  null,
  );

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
} # end class Sensor_Server_Type
?>
