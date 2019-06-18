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
  console.log("UPDtate buttons");

  var canSave = true;
  if ( form.elements['newAction[SensorId]'] && !form.elements['newAction[SensorId]'].value ) {
    canSave = false;
  } else if ( form.elements['newAction[Action]'] && !form.elements['newAction[Action]'].value ) {
    canSave = false;
  }
  $j('#SaveButton')[0].disabled = !canSave;
}

function init() {
  updateButtons( $('SaveButton') );
}

window.addEventListener( 'DOMContentLoaded', init );

