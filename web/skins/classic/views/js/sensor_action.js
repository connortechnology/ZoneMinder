function validateForm(form) {
  var errors = [];
  if ( !form.elements['newAction[SensorId]'].value ) {
    errors[errors.length] = 'You must choose a sensor';
  }
  if ( errors.length ) {
    alert(errors.join("\n"));
    return false;
  }
  return true;
}
function updateButtons(element) {
  var form = element.form;
  if ( ! form ) {
    form = $j('[name="contentForm"]');
    if ( ! form ) {
      console.log('No form found.');
      return;
    }
    form = form[0];
  }
  console.log(form);
  var elements = form.elements;
  console.log(elements);

  var canSave = true;
  if ( !elements['newAction[SensorId]'] ) {
    console.log('No SensorId');
    canSave = false;
  } else if ( !( elements['newAction[SensorId]'].selectedIndex > 0 ) ) {
    console.log('No SensorId value' + elements['newAction[SensorId]'].selectedIndex );
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

