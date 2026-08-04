<?php
ini_set('display_errors', '0');

if ( canView('Monitors') || (isset($_REQUEST['mid']) && $_REQUEST['mid'] !== '' && canView('Monitors', $_REQUEST['mid'])) ) {
  $mid = isset($_REQUEST['mid']) ? $_REQUEST['mid'] : null;
  if ($mid === null || $mid === '') {
    ajaxError(translate('RequestMissing') . ' "mid".');
  }

  $action = $_REQUEST['action'] ?? '';
  if ($action === '') {
    ajaxError(translate('RequestMissing') . ' "action".');
  }

  switch ( $action ) {
  case 'validateName' :
    require_once('includes/Monitor.php');
    $monitor = new ZM\Monitor($mid);
    $filterRegexp = $monitor->getDefaults()['Name']['filter_regexp'];
    $result = true;
    $badChars = [];
    $message = '';

    if (isset($_REQUEST['monitorName']) && is_string($_REQUEST['monitorName']) && $_REQUEST['monitorName'] !== '') {
      $monitorName = $_REQUEST['monitorName'];
      $trimmedMonitorName = trim($monitorName);
      $cleanedMonitorName = preg_replace($filterRegexp, '', $trimmedMonitorName);
      if ($trimmedMonitorName != $cleanedMonitorName){
        preg_match_all($filterRegexp, $trimmedMonitorName, $badChars);
        $result = false;
        $message = translate('BadNameCharsList') . ' "' . implode('","', array_unique($badChars[0])) . '".~~' . translate('BadNameChars');
      }
      ajaxResponse(array('response'=>$result, 'monitorName'=>$monitorName, 'cleanedMonitorName'=>$cleanedMonitorName, 'badChars'=>$badChars, 'messageBadNameChars'=>$message));
    } else {
      ajaxError(translate('ErrorVerifyingMonitorName'));
    }
    break;

  case 'validateFilePath' :
    // Reporting whether a path exists is a filesystem probe, so require edit
    // rights rather than the view rights that gate the rest of this endpoint.
    if (!canEdit('Monitors', $mid)) {
      ajaxError(translate('insufficientPermissionsUser').' "'.validHtmlStr($user->Username()).'"');
    }
    $path = $_REQUEST['path'] ?? '';
    if (!is_string($path) or $path === '') {
      ajaxError(translate('RequestMissing').' "path".');
    }
    if (!preg_match('#^file://#i', $path)) {
      // Not a local file url, so there is nothing here to check.
      ajaxResponse(array('checked'=>false));
    }
    // file:///path and file://localhost/path both denote a local path.
    $file = urldecode(preg_replace('#^file://(localhost)?#i', '', $path));
    ajaxResponse(array(
      'checked' => true,
      'exists' => file_exists($file),
      'readable' => is_readable($file),
    ));
    break;
  } // end switch action
} // end if canView('Monitors')

ajaxError(translate('UnrecognisedAction').' "'.validHtmlStr($_REQUEST['action'] ?? '').'" '.translate('ConjOr').' '.translate('insufficientPermissionsUser').' "'.validHtmlStr($user->Username()).'"');
?>
