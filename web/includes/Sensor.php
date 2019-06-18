<?php
namespace ZM;
require_once('database.php');
require_once('Sensor_Server.php');
require_once('Sensor_Action.php');

$sensor_cache = array();

class Sensor {
  private $defaults = array(
    'Id'                   => null,
    'SensorId'             => '',
    'SensorServerId'       => null,
    'Name'                 => '',
  );

  public function __construct($IdOrRow = NULL) {
    global $sensor_cache;
    $row = NULL;
    if ( $IdOrRow ) {
      if ( is_integer($IdOrRow) or ctype_digit($IdOrRow) ) {
        $row = dbFetchOne('SELECT * FROM Sensors WHERE Id=?', NULL, array($IdOrRow));
        if ( !$row ) {
          Error('Unable to load Sensor record for Id='.$IdOrRow);
        }
      } elseif ( is_array($IdOrRow) ) {
        $row = $IdOrRow;
      }
    } # end if isset($IdOrRow)
    if ( $row ) {
      foreach ($row as $k => $v) {
        $this->{$k} = $v;
      }
      $sensor_cache[$row['Id']] = $this;
    } else {
      # Set defaults
      foreach ( $this->defaults as $k => $v ) $this->{$k} = $v;
    }
  }

  public function __call($fn, array $args){
    if ( count($args) ) {
      $this->{$fn} = $args[0];
    }
    if ( array_key_exists($fn, $this) ) {
      return $this->{$fn};
    } else {
      if ( array_key_exists($fn, $this->defaults) ) {
        return $this->defaults{$fn};
      } else {
        $backTrace = debug_backtrace();
        $file = $backTrace[1]['file'];
        $line = $backTrace[1]['line'];
        Warning("Unknown function call Sensor->$fn from $file:$line");
      }
    }
  }

  public static function find( $parameters = null, $options = null ) {
    $filters = array();
    $sql = 'SELECT * FROM Sensors ';
    $values = array();

    if ( $parameters ) {
      $fields = array();
      $sql .= 'WHERE ';
      foreach ( $parameters as $field => $value ) {
        if ( $value == null ) {
          $fields[] = $field.' IS NULL';
        } else if ( is_array( $value ) ) {
          $func = function(){return '?';};
          $fields[] = $field.' IN ('.implode(',', array_map( $func, $value ) ). ')';
          $values += $value;

        } else {
          $fields[] = $field.'=?';
          $values[] = $value;
        }
      }
      $sql .= implode(' AND ', $fields );
    }
    if ( $options ) {
      if ( isset($options['order']) ) {
        $sql .= ' ORDER BY ' . $options['order'];
      }
      if ( isset($options['limit']) ) {
        if ( is_integer($options['limit']) or ctype_digit($options['limit']) ) {
          $sql .= ' LIMIT ' . $options['limit'];
        } else {
          $backTrace = debug_backtrace();
          $file = $backTrace[1]['file'];
          $line = $backTrace[1]['line'];
          Error('Invalid value for limit('.$options['limit'].') passed to '.get_class()."::find from $file:$line");
          return array();
        }
      }
    }
    $results = dbFetchAll($sql, NULL, $values);
    if ( $results ) {
      return array_map(function($id){ 
        $class = get_class();
        return new $class($id);
      }, $results);
    }
    return array();
  } # end public function find()

  public static function find_one( $parameters = array() ) {
    global $sensor_cache;
    if ( 
        ( count($parameters) == 1 ) and
        isset($parameters['Id']) and
        isset($sensor_cache[$parameters['Id']]) ) {
      return $sensor_cache[$parameters['Id']];
    }
    $class = get_class();
    $results = $class::find($parameters, array('limit'=>1));
    if ( ! sizeof($results) ) {
      return;
    }
    return $results[0];
  }

  public function to_json() {
    $json = array();
    foreach ($this->defaults as $key => $value) {
      if ( is_callable(array($this, $key)) ) {
        $json[$key] = $this->$key();
      } else if ( array_key_exists($key, $this) ) {
        $json[$key] = $this->{$key};
      } else {
        $json[$key] = $this->defaults{$key};
      }
    }
    return json_encode($json);
  }

  public function Actions($new=-1) {
    if ( $new != -1 ) {
      $this->{'Actions'} = $new;
    }

    if ( !(array_key_exists('Actions', $this) and $this->{'Actions'}) ) {
      $this->{'Actions'} = Sensor_Action::find(array('SensorId'=>$this->Id()));
    }
    Error("Have " .count($this->{'Actions'}). " Actions" );
    return $this->{'Actions'};
  } # end public function Actions

  static function Objects_Indexed_By_Id() {
    $Objects = array();
    foreach ( Sensor::find(null, array('order'=>'lower(Name)')) as $Object ) {
      $Objects[$Object->Id()] = $Object;
    }
    return $Objects;
  }
} # end class Sensor
?>
