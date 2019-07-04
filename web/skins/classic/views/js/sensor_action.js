function validateForm(form) {
  var errors = [];
  if ( !form.elements['newAction[MinSensorId]'].value ) {
    errors[errors.length] = 'You must choose a minimum sensor';
  }
  if ( !form.elements['newAction[MaxSensorId]'].value ) {
    errors[errors.length] = 'You must choose a maximum sensor';
  }
  if ( errors.length ) {
    alert(errors.join("\n"));
    return false;
  }
  return true;
}
function updateButtons(element) {
  var form = element.form;
  if ( !form ) {
    form = $j('[name="contentForm"]');
    if ( !form ) {
      console.log('No form found.');
      return;
    }
    form = form[0];
  }
  var elements = form.elements;

  var canSave = true;
  if ( !elements['newAction[MinSensorId]'] ) {
    console.log('No Min SensorId');
    canSave = false;
  } else if ( !( elements['newAction[MinSensorId]'].selectedIndex > 0 ) ) {
    console.log('No Min SensorId value' + elements['newAction[MinSensorId]'].selectedIndex);
    canSave = false;
  } else if ( !( elements['newAction[MaxSensorId]'].selectedIndex > 0 ) ) {
    console.log('No Max SensorId value' + elements['newAction[MaxSensorId]'].selectedIndex);
    canSave = false;
  } else if ( !( elements['newAction[MonitorId]'].selectedIndex > 0 ) ) {
    console.log('No MonitorId value' + elements['newAction[MonitorId]'].selectedIndex );
    canSave = false;
  } else if ( !( elements['newAction[TypeId]'].selectedIndex > 0 ) ) {
    console.log('No TypeId value' + elements['newAction[TypeId]'].selectedIndex );
    canSave = false;
  } else if ( elements['newAction[Action]'] && !elements['newAction[Action]'].value ) {
    canSave = false;
    console.log('No action');
  }
  $j('#SaveButton')[0].disabled = !canSave;
}

function init() {
  updateButtons( $('SaveButton') );
}

window.addEventListener( 'DOMContentLoaded', init );

