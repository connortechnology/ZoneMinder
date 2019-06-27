<?php
namespace ZM;
require_once('database.php');
require_once('Object.php');

$sensor_server_cache = array();

class Sensor_Action_Type extends ZM_Object {
  protected static $table = 'Sensor_Action_Types';
  private $defaults = array(
    'Id'     => null,
    'TypeId' => '',
    'Name'   => '',
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
      $Objects[$Object->TypeId()] = $Object;
    }
    return $Objects;
  }
} # end class Sensor_Server
?>
