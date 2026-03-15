<?php
//
// ZoneMinder Quadra AJAX handler
//

if (!canView('System')) {
  ajaxError('Insufficient permissions');
  return;
}

require_once('includes/Quadra.php');

$data = buildQuadraData();

if (isset($data['error'])) {
  ajaxError($data['error']);
  return;
}

ajaxResponse($data);
