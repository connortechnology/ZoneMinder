<?php
namespace ZM;
require_once('database.php');
require_once('Monitor.php');

$sensor_action_cache = array();

class Sensor_Action {
  private $defaults = array(
    'Id'          => null,
    'Name'        =>  '',
    'SensorId'    => null,
    'MonitorId'   => null,
    'TypeId'      => null,
    'MinValue'    =>  null,
    'MaxValue'    =>  null,
    'Action'      =>  '',
  );

  public function __construct($IdOrRow = NULL) {
    global $sensor_action_cache;
    $row = NULL;
    if ( $IdOrRow ) {
      if ( is_integer($IdOrRow) or ctype_digit($IdOrRow) ) {
        $row = dbFetchOne('SELECT * FROM Sensor_Actions WHERE Id=?', NULL, array($IdOrRow));
        if ( !$row ) {
          Error('Unable to load Sensor Action record for Id='.$IdOrRow);
        }
      } elseif ( is_array($IdOrRow) ) {
        $row = $IdOrRow;
      }
    } # end if isset($IdOrRow)
    if ( $row ) {
      foreach ($row as $k => $v) {
        $this->{$k} = $v;
      }
      $sensor_action_cache[$row['Id']] = $this;
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
    $sql = 'SELECT * FROM Sensor_Actions ';
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
    global $sensor_action_cache;
    if ( 
        ( count($parameters) == 1 ) and
        isset($parameters['Id']) and
        isset($sensor_action_cache[$parameters['Id']]) ) {
      return $sensor_action_cache[$parameters['Id']];
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

  public function set($data) {
    foreach ($data as $k => $v) {
      if ( method_exists($this, $k) ) {
        $this->{$k}($v);
      } else {
        if ( is_array( $v ) ) {
# perhaps should turn into a comma-separated string
          $this->{$k} = implode(',',$v);
        } else if ( is_string( $v ) ) {
          $this->{$k} = trim( $v );
        } else if ( is_integer( $v ) ) {
          $this->{$k} = $v;
        } else if ( is_bool( $v ) ) {
          $this->{$k} = $v;
        } else if ( is_null( $v ) ) {
          $this->{$k} = $v;
        } else {
          Error( "Unknown type $k => $v of var " . gettype( $v ) );
          $this->{$k} = $v;
        }
      } # end if method_exists
    } # end foreach $data as $k=>$v
  }

  public function changes( $new_values ) {
    $changes = array();
    foreach ( $this->defaults as $field=>$default_value ) {
      if ( array_key_exists($field, $new_values) ) {
      Logger::Debug("Checking default $field => $default_value exists in new values");
        if ( (!array_key_exists($field, $this)) or ( $this->{$field} != $new_values[$field] ) ) {
      Logger::Debug("Checking default $field => $default_value changes becaause" . $new_values[$field].' != '.$new_values[$field]);
          $changes[$field]=$new_values[$field];
        #} else if  {
      #Logger::Debug("Checking default $field => $default_value changes becaause " . $new_values[$field].' != '.$new_values[$field]);
          #array_push( $changes, [$field=>$defaults[$field]] );
        }
      } else {
        Logger::Debug("Checking default $field => $default_value not in new_values");
      }
    } # end foreach default 
    return $changes;
  } # end public function changes

  public function save($new_values = null) {

    if ( $new_values ) {
      Logger::Debug("New values" . print_r($new_values,true));
      $this->set($new_values);
    }

    if ( $this->Id() ) {
      $fields = array_keys($this->defaults);
      $sql = 'UPDATE Sensor_Actions SET '.implode(', ', array_map(function($field) {return '`'.$field.'=?'.'`';}, $fields )) . ' WHERE Id=?';
      $values = array_map(function($field){return $this->{$field};}, $fields);
      $values[] = $this->{'Id'};
      if ( dbQuery($sql, $values) )
        return true;
    } else {
      $fields = $this->defaults;
      unset($fields['Id']);

      $sql = 'INSERT INTO Sensor_Actions ('.implode(', ', array_map(function($field) {return '`'.$field.'`';}, array_keys($fields))).') VALUES ('.implode(', ', array_map(function($field){return '?';}, array_values($fields))).')';
      $values = array_map(function($field){return $this->{$field};}, array_keys($fields));
      if ( dbQuery($sql, $values) ) {
        $this->{'Id'} = dbInsertId();
        return true;
      }
    }
    return false;
  } // end function save

} # end class Sensor Action
?>
