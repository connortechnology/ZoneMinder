<?php
//
// Shared helper functions for Quadra (NetInt) hardware monitoring.
// Used by the quadra view and its AJAX handler.
//

// Resolve the full path to ni_rsrc_mon.  The web server's PATH may not
// include /usr/local/bin where the tool is typically installed.
function findNiRsrcMon() {
  static $path = null;
  if ($path !== null) return $path;

  $candidates = [
    '/usr/local/bin/ni_rsrc_mon',
    '/usr/bin/ni_rsrc_mon',
  ];
  foreach ($candidates as $candidate) {
    if (is_executable($candidate)) {
      $path = $candidate;
      return $path;
    }
  }

  $path = 'ni_rsrc_mon';
  return $path;
}

// ni_rsrc_mon -o json outputs non-standard JSON: unquoted hex strings
// (e.g. "SID": 9e48), unquoted barewords ("Format": YUV), and trailing
// commas after the last array element.  Fix these before json_decode.
function fixQuadraJson($text) {
  // Remove trailing commas before ] or }
  $text = preg_replace('/,(\s*[}\]])/', '$1', $text);

  // Handle empty values: "KEY":  , or "KEY":  } — replace with null
  $text = preg_replace('/"([\w]+)"\s*:\s*([,}\]])/', '"$1": null$2', $text);

  // Quote unquoted values (barewords, hex strings, multi-word values).
  // Matches "KEY": VALUE where VALUE is everything up to the next , or } or ]
  // that isn't already a quoted string, array, object, or valid JSON literal.
  $text = preg_replace_callback(
    '/"([\w]+)"\s*:\s*([^\s"\[\]{},](?:[^,\]\}]*[^\s,\]\}])?)(\s*[,}\]])/',
    function ($matches) {
      $value = trim($matches[2]);
      // Leave valid JSON literals alone
      if (preg_match('/^-?\d+$/', $value)) return $matches[0];
      if (preg_match('/^-?\d+\.\d+$/', $value)) return $matches[0];
      if (in_array($value, ['true', 'false', 'null'])) return $matches[0];
      return '"'.$matches[1].'": "'.$value.'"'.$matches[3];
    },
    $text
  );

  return $text;
}

// Parse ni_rsrc_mon JSON output.  The tool emits a timestamp header
// followed by multiple separate JSON objects { "section": [...] },
// delimited by **** lines.
function parseQuadraJsonOutput($args) {
  $bin = findNiRsrcMon();
  $command = $bin.' '.$args;

  if (!function_exists('exec') || in_array('exec', array_map('trim', explode(',', ini_get('disable_functions'))))) {
    ZM\Warning("Quadra: exec() is disabled in PHP configuration");
    return ['error' => 'exec() is disabled in PHP configuration. Cannot run ni_rsrc_mon.'];
  }

  $output = [];
  $returnCode = -1;
  exec($command.' 2>&1', $output, $returnCode);

  $outputText = implode("\n", $output);
  $lineCount = count($output);

  if ($returnCode != 0) {
    ZM\Warning("Quadra: '$command' failed with return code $returnCode: $outputText");
    if ($returnCode == 127 || (empty($output) && $returnCode != 0)) {
      $diag = "Command '$command' not found (return code $returnCode).";
      if ($bin === 'ni_rsrc_mon') {
        $diag .= ' ni_rsrc_mon was not found in /usr/local/bin or /usr/bin.'
                 .' Ensure the NetInt tools are installed.';
      }
      return ['error' => $diag];
    }
    return ['error' => "Command failed (return code $returnCode): $outputText"];
  }

  if (empty($output)) {
    ZM\Warning("Quadra: '$command' returned no output (return code $returnCode)");
    return ['error' => "ni_rsrc_mon returned no output. Command: $command"];
  }

  ZM\Debug("Quadra: '$command' returned $lineCount lines (rc=$returnCode)");

  $result = ['timestamp' => '', 'uptime' => '', 'version' => ''];
  $rawText = $outputText;

  if (preg_match('/(\w{3}\s+\w{3}\s+\d+\s+[\d:]+\s+\d{4})\s+up\s+([\d:]+)\s+v(.+)/', $rawText, $matches)) {
    $result['timestamp'] = $matches[1];
    $result['uptime'] = $matches[2];
    $result['version'] = $matches[3];
  }

  $sectionCount = 0;
  $parseErrors = [];
  if (preg_match_all('/^\{\s*"(\w+)"\s*:.*?^\}/ms', $rawText, $jsonBlocks, PREG_SET_ORDER)) {
    foreach ($jsonBlocks as $block) {
      $key = $block[1];
      $json = fixQuadraJson($block[0]);
      $parsed = json_decode($json, true);
      if ($parsed !== null && isset($parsed[$key])) {
        if (!isset($result[$key])) $result[$key] = [];
        $result[$key] = array_merge($result[$key], $parsed[$key]);
        $sectionCount++;
      } else {
        $jsonError = json_last_error_msg();
        $parseErrors[] = "$key: $jsonError";
        ZM\Warning("Quadra: JSON parse error for section '$key': $jsonError. Fixed JSON: ".substr($json, 0, 1000));
      }
    }
  } else {
    ZM\Warning("Quadra: no JSON sections found in output of '$command'. "
           ."Output ($lineCount lines): ".substr($rawText, 0, 500));
    return ['error' => "ni_rsrc_mon output could not be parsed. "
           ."No JSON sections found in $lineCount lines of output. "
           ."First 200 chars: ".htmlspecialchars(substr($rawText, 0, 200))];
  }

  if (!empty($parseErrors)) {
    ZM\Warning('Quadra: failed to parse '.count($parseErrors).' section(s): '.implode('; ', $parseErrors));
  }
  if ($sectionCount > 0) {
    ZM\Debug("Quadra: parsed $sectionCount sections from '$command'");
  }

  return $result;
}

function formatQuadraBitrate($bps) {
  $bps = intval($bps);
  if ($bps >= 1000000) return number_format($bps / 1000000, 1).' Mbps';
  if ($bps >= 1000) return number_format($bps / 1000, 1).' kbps';
  return number_format($bps).' bps';
}

// Section definitions
function getQuadraResourceSections() {
  return [
    'decoder'  => ['label' => 'Decoder',  'icon' => 'input'],
    'encoder'  => ['label' => 'Encoder',  'icon' => 'output'],
    'uploader' => ['label' => 'Uploader', 'icon' => 'cloud_upload'],
    'scaler'   => ['label' => 'Scaler',   'icon' => 'aspect_ratio'],
    'AI'       => ['label' => 'AI',       'icon' => 'psychology'],
  ];
}

function getQuadraInfraSections() {
  return [
    'nvme' => ['label' => 'NVMe',      'icon' => 'storage'],
    'tp'   => ['label' => 'Transport', 'icon' => 'swap_horiz'],
    'pcie' => ['label' => 'PCIe',      'icon' => 'settings_ethernet'],
  ];
}

// Build per-card data from summary + detailed output
function buildQuadraCards($quadraSummary, $quadraDetailed) {
  $resourceSections = getQuadraResourceSections();
  $infraSections = getQuadraInfraSections();

  $cards = [];
  $allSections = array_merge(array_keys($resourceSections), array_keys($infraSections));
  foreach ($allSections as $sect) {
    if (empty($quadraSummary[$sect])) continue;
    foreach ($quadraSummary[$sect] as $entry) {
      $idx = $entry['INDEX'] ?? 0;
      if (!isset($cards[$idx])) {
        $cards[$idx] = [
          'device' => $entry['DEVICE'] ?? 'N/A',
          'pcie_addr' => $entry['PCIE_ADDR'] ?? 'N/A',
          'numa_node' => $entry['NUMA_NODE'] ?? '?',
          'firmware' => $entry['FR'] ?? ($quadraSummary['version'] ?? 'N/A'),
          'summary' => [],
          'detailed' => [],
        ];
      }
      if (!isset($cards[$idx]['summary'][$sect])) $cards[$idx]['summary'][$sect] = [];
      $cards[$idx]['summary'][$sect][] = $entry;
    }
  }

  // Build DEVICE -> card index lookup from summary data
  $deviceToCard = [];
  foreach ($cards as $idx => $card) {
    $deviceToCard[$card['device']] = $idx;
  }

  // Assign detailed session data to cards by matching DEVICE field
  foreach (['decoder', 'encoder'] as $sect) {
    if (empty($quadraDetailed[$sect])) continue;
    foreach ($quadraDetailed[$sect] as $entry) {
      $dev = $entry['DEVICE'] ?? '';
      if (isset($deviceToCard[$dev])) {
        $idx = $deviceToCard[$dev];
      } else {
        $idx = array_key_first($cards);
        if ($idx === null) continue;
      }
      if (!isset($cards[$idx]['detailed'][$sect])) $cards[$idx]['detailed'][$sect] = [];
      $cards[$idx]['detailed'][$sect][] = $entry;
    }
  }

  ksort($cards);
  return $cards;
}

// Build VPU chart data array for ECharts
function buildVpuChartData($cards, $multiCard) {
  $resourceSections = getQuadraResourceSections();
  $vpuChartData = [];
  foreach ($cards as $cardIdx => $card) {
    foreach ($resourceSections as $key => $meta) {
      if (empty($card['summary'][$key])) continue;
      $entry = $card['summary'][$key][0];
      $totalMem = intval($entry['MEM'] ?? 0) + intval($entry['CRITICAL_MEM'] ?? 0) + intval($entry['SHARE_MEM'] ?? 0);
      $label = $multiCard ? 'Card '.$cardIdx.' '.$meta['label'] : $meta['label'];
      $vpuChartData[] = [
        'label' => $label,
        'load' => intval($entry['LOAD'] ?? 0),
        'mem' => $totalMem > 0 ? intval($entry['MEM'] ?? 0) : 0,
      ];
    }
  }
  return $vpuChartData;
}

// Gather Host Usage data (CPU, RAM, Storage)
function buildHostUsageData() {
  $hostUsage = [];

  // CPU
  global $thisServer;
  if ($thisServer) {
    $thisServer->ReadStats();
    $cpuPercent = round($thisServer->CpuUsagePercent, 1);
    $cpuThreads = getcpus();
    $hostUsage[] = [
      'label' => 'CPU',
      'value' => $cpuPercent,
      'info' => $cpuThreads.' Threads',
    ];
  }

  // RAM
  if (file_exists('/proc/meminfo')) {
    $contents = file_get_contents('/proc/meminfo');
    preg_match_all('/(\w+):\s+(\d+)\s/', $contents, $matches);
    $meminfo = array_combine($matches[1], array_map(function($v){return 1024*$v;}, $matches[2]));
    if (isset($meminfo['MemTotal']) && $meminfo['MemTotal'] > 0) {
      $mem_used = $meminfo['MemTotal'] - $meminfo['MemFree'] - $meminfo['Buffers'] - $meminfo['Cached'];
      $mem_used_percent = round(100 * $mem_used / $meminfo['MemTotal'], 2);
      $mem_total_human = human_filesize($meminfo['MemTotal'], 2);
      $hostUsage[] = [
        'label' => 'RAM',
        'value' => $mem_used_percent,
        'info' => $mem_total_human,
      ];
    }
  }

  // Storage areas
  $storage_areas = ZM\Storage::find(['Enabled' => true]);
  foreach ($storage_areas as $area) {
    $disk_total = $area->disk_total_space();
    if ($disk_total > 0) {
      $hostUsage[] = [
        'label' => 'Disk ('.$area->Name().')',
        'value' => $area->disk_usage_percent(),
        'info' => human_filesize($disk_total, 1),
      ];
    }
  }

  return $hostUsage;
}

// Build full JSON-serializable data for AJAX responses
function buildQuadraData() {
  $quadraSummary = parseQuadraJsonOutput('-o json');
  $quadraDetailed = parseQuadraJsonOutput('-d -o json');

  if (isset($quadraSummary['error'])) {
    return ['error' => $quadraSummary['error']];
  }

  $cards = buildQuadraCards($quadraSummary, $quadraDetailed);
  $multiCard = count($cards) > 1;
  $resourceSections = getQuadraResourceSections();
  $infraSections = getQuadraInfraSections();

  // Build cards data for table rendering (strip raw summary/detailed, provide display-ready data)
  $cardsData = [];
  foreach ($cards as $cardIdx => $card) {
    $cardOut = [
      'device' => $card['device'],
      'pcie_addr' => $card['pcie_addr'],
      'numa_node' => $card['numa_node'],
      'firmware' => $card['firmware'],
      'resources' => [],
      'infra' => [],
      'decoders' => [],
      'encoders' => [],
    ];

    // Resources
    foreach ($resourceSections as $key => $meta) {
      if (empty($card['summary'][$key])) continue;
      foreach ($card['summary'][$key] as $entry) {
        $cardOut['resources'][] = [
          'label' => $meta['label'],
          'icon' => $meta['icon'],
          'load' => $entry['LOAD'] ?? '0',
          'model_load' => $entry['MODEL_LOAD'] ?? '0',
          'fw_load' => $entry['FW_LOAD'] ?? '0',
          'inst' => $entry['INST'] ?? '0',
          'max_inst' => $entry['MAX_INST'] ?? '0',
          'mem' => $entry['MEM'] ?? '0',
          'critical_mem' => $entry['CRITICAL_MEM'] ?? '0',
          'share_mem' => $entry['SHARE_MEM'] ?? '0',
        ];
      }
    }

    // Infrastructure
    foreach ($infraSections as $key => $meta) {
      if (empty($card['summary'][$key])) continue;
      foreach ($card['summary'][$key] as $entry) {
        $cardOut['infra'][] = [
          'label' => $meta['label'],
          'icon' => $meta['icon'],
          'fw_load' => $entry['FW_LOAD'] ?? '0',
          'share_mem' => $entry['SHARE_MEM'] ?? '0',
          'pcie_throughput' => $entry['PCIE_THROUGHPUT'] ?? null,
        ];
      }
    }

    // Decoder sessions
    if (!empty($card['detailed']['decoder'])) {
      foreach ($card['detailed']['decoder'] as $d) {
        $cardOut['decoders'][] = [
          'index' => $d['INDEX'] ?? '',
          'width' => $d['Width'] ?? '',
          'height' => $d['Height'] ?? '',
          'fps' => $d['FrameRate'] ?? '',
          'avg_cost' => $d['AvgCost'] ?? '',
          'idr' => $d['IDR'] ?? '',
          'in_frames' => intval($d['InFrame'] ?? 0),
          'out_frames' => intval($d['OutFrame'] ?? 0),
          'sid' => $d['SID'] ?? '',
        ];
      }
    }

    // Encoder sessions
    if (!empty($card['detailed']['encoder'])) {
      foreach ($card['detailed']['encoder'] as $e) {
        $cardOut['encoders'][] = [
          'index' => $e['INDEX'] ?? '',
          'width' => $e['Width'] ?? '',
          'height' => $e['Height'] ?? '',
          'format' => $e['Format'] ?? '',
          'fps' => $e['FrameRate'] ?? '',
          'bitrate' => formatQuadraBitrate($e['BR'] ?? 0),
          'avg_bitrate' => formatQuadraBitrate($e['AvgBR'] ?? 0),
          'avg_cost' => $e['AvgCost'] ?? '',
          'idr' => $e['IDR'] ?? '',
          'in_frames' => intval($e['InFrame'] ?? 0),
          'out_frames' => intval($e['OutFrame'] ?? 0),
          'sid' => $e['SID'] ?? '',
        ];
      }
    }

    $cardsData[$cardIdx] = $cardOut;
  }

  return [
    'timestamp' => $quadraSummary['timestamp'] ?? '',
    'uptime' => $quadraSummary['uptime'] ?? '',
    'vpuChartData' => buildVpuChartData($cards, $multiCard),
    'hostUsage' => buildHostUsageData(),
    'cards' => $cardsData,
    'multiCard' => $multiCard,
  ];
}
