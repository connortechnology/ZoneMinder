<?php
namespace ZM;
require_once('database.php');
require_once('Sensor.php');

$sensor_server_cache = array();

class Sensor_Server {
  private $defaults = array(
    'Id'                   => null,
    'Name'                 => '',
    'Url'                  => '',
  );

  public function __construct($IdOrRow = NULL) {
    global $sensor_server_cache;
    $row = NULL;
    if ( $IdOrRow ) {
      if ( is_integer($IdOrRow) or ctype_digit($IdOrRow) ) {
        $row = dbFetchOne('SELECT * FROM Sensor_Servers WHERE Id=?', NULL, array($IdOrRow));
        if ( !$row ) {
          Error('Unable to load Sensor_Server record for Id='.$IdOrRow);
        }
      } elseif ( is_array($IdOrRow) ) {
        $row = $IdOrRow;
      }
    } # end if isset($IdOrRow)
    if ( $row ) {
      foreach ($row as $k => $v) {
        $this->{$k} = $v;
      }
      $sensor_server_cache[$row['Id']] = $this;
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
        Warning("Unknown function call Sensor_Server->$fn from $file:$line");
      }
    }
  }

  public function Sensors($new=-1) {
    if ( $new != -1 ) {
      $this->{'Sensors'} = $new;
    }

    if ( !array_key_exists('Sensors', $this) ) {
      $this->{'Sensors'} = Sensor::find(array('SensorServerId'=>$this->Id()));
    }
    return $this->{'Sensors'};
  } # end public function Sensors()

  public static function find( $parameters = null, $options = null ) {
    $filters = array();
    $sql = 'SELECT * FROM Sensor_Servers ';
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
    global $sensor_server_cache;
    if ( 
        ( count($parameters) == 1 ) and
        isset($parameters['Id']) and
        isset($sensor_server_cache[$parameters['Id']]) ) {
      return $sensor_server_cache[$parameters['Id']];
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

} # end class Sensor_Server
?>
