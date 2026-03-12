<?php
//
// ZoneMinder web Quadra status view file
// Copyright (C) 2024 ZoneMinder Inc.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
//

if (!canView('System')) {
  $view = 'error';
  return;
}

// Resolve the full path to ni_rsrc_mon.  The web server's PATH may not
// include /usr/local/bin where the tool is typically installed.
function findNiRsrcMon() {
  static $path = null;
  if ($path !== null) return $path;

  // Check common locations
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

  // Fall back to bare command (relies on PATH)
  $path = 'ni_rsrc_mon';
  return $path;
}

// ni_rsrc_mon -o json outputs non-standard JSON: unquoted hex strings
// (e.g. "SID": 9e48), unquoted barewords ("Format": YUV), and trailing
// commas after the last array element.  Fix these before json_decode.
function fixQuadraJson($text) {
  // Remove trailing commas before ] or }
  $text = preg_replace('/,(\s*[}\]])/', '$1', $text);

  // Quote unquoted values (barewords and hex strings).
  // Matches "KEY": VALUE where VALUE is not already quoted, not an array/object,
  // and not a pure integer or float.
  $text = preg_replace_callback(
    '/("[\w]+"\s*:\s*)([^\s"\[\]{},][^\s,\]\}]*)(\s*[,}\]])/',
    function ($matches) {
      $value = trim($matches[2]);
      if (preg_match('/^-?\d+$/', $value)) return $matches[0];
      if (preg_match('/^-?\d+\.\d+$/', $value)) return $matches[0];
      if (in_array($value, ['true', 'false', 'null'])) return $matches[0];
      return $matches[1].'"'.$value.'"'.$matches[3];
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

  // Check if exec() is available
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
      // Command not found - provide helpful diagnostics
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

  // Each section is { "key": [...] } with the outer } at column 0.
  // Inner object braces are tab-indented, so ^\} only matches the outer close.
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
        ZM\Warning("Quadra: JSON parse error for section '$key': $jsonError");
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

function formatBitrate($bps) {
  $bps = intval($bps);
  if ($bps >= 1000000) return number_format($bps / 1000000, 1).' Mbps';
  if ($bps >= 1000) return number_format($bps / 1000, 1).' kbps';
  return number_format($bps).' bps';
}

$quadraSummary = parseQuadraJsonOutput('-o json');
$quadraDetailed = parseQuadraJsonOutput('-d -o json');

$resourceSections = [
  'decoder'  => ['label' => 'Decoder',  'icon' => 'input'],
  'encoder'  => ['label' => 'Encoder',  'icon' => 'output'],
  'uploader' => ['label' => 'Uploader', 'icon' => 'cloud_upload'],
  'scaler'   => ['label' => 'Scaler',   'icon' => 'aspect_ratio'],
  'AI'       => ['label' => 'AI',       'icon' => 'psychology'],
];

$infraSections = [
  'nvme' => ['label' => 'NVMe',      'icon' => 'storage'],
  'tp'   => ['label' => 'Transport', 'icon' => 'swap_horiz'],
  'pcie' => ['label' => 'PCIe',      'icon' => 'settings_ethernet'],
];

// Build per-card data indexed by card INDEX
// Each entry in the summary/detailed arrays has an INDEX field identifying the card
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
// Assign detailed session data to cards by matching DEVICE field.
// In detailed output INDEX is the session number, not the card index.
foreach (['decoder', 'encoder'] as $sect) {
  if (empty($quadraDetailed[$sect])) continue;
  foreach ($quadraDetailed[$sect] as $entry) {
    $dev = $entry['DEVICE'] ?? '';
    if (isset($deviceToCard[$dev])) {
      $idx = $deviceToCard[$dev];
    } else {
      // Fall back to first card if DEVICE not found
      $idx = array_key_first($cards);
      if ($idx === null) continue;
    }
    if (!isset($cards[$idx]['detailed'][$sect])) $cards[$idx]['detailed'][$sect] = [];
    $cards[$idx]['detailed'][$sect][] = $entry;
  }
}
ksort($cards);
$hasData = !empty($cards);

xhtmlHeaders(__FILE__, 'Quadra Status');
getBodyTopHTML();
echo getNavBarHTML();
?>
<div id="page" class="container-fluid">
<?php echo getPageHeaderHTML() ?>

  <div id="toolbar" class="pb-2">
    <button id="backBtn" class="btn btn-normal" data-toggle="tooltip" data-placement="top" title="<?php echo translate('Back') ?>" disabled><i class="fa fa-arrow-left"></i></button>
    <button id="refreshBtn" class="btn btn-normal" data-toggle="tooltip" data-placement="top" title="<?php echo translate('Refresh') ?>"><i class="fa fa-refresh"></i></button>
  </div>

  <div id="content">
<?php if (isset($quadraSummary['error'])): ?>
  <div class="alert alert-danger">
    <strong>Error running ni_rsrc_mon:</strong><br>
    <?php echo htmlspecialchars($quadraSummary['error']) ?>
  </div>
<?php elseif (!$hasData): ?>
  <div class="alert alert-warning">
    <strong>No data:</strong> ni_rsrc_mon returned no resource data.
    Check that a NetInt Quadra device is installed and that the web server user has permission to access it.
  </div>
<?php else: ?>

  <!-- VPU Devices -->
  <div class="card mb-3">
    <div class="card-header">
      <i class="material-icons md-18">memory</i> VPU Devices
      <span class="text-muted small ml-2">
        <?php echo htmlspecialchars($quadraSummary['timestamp'] ?: '') ?>
        <?php if ($quadraSummary['uptime']) echo '&mdash; up '.htmlspecialchars($quadraSummary['uptime']) ?>
      </span>
    </div>
    <div class="card-body table-responsive">
      <table class="table table-sm table-hover">
        <thead class="text-left">
          <tr>
            <th>Device</th>
            <th>Firmware</th>
            <th>PCIe Address</th>
            <th>NUMA</th>
          </tr>
        </thead>
        <tbody>
<?php foreach ($cards as $cardIdx => $card): ?>
          <tr>
            <td><?php echo htmlspecialchars($card['device']) ?></td>
            <td><?php echo htmlspecialchars($card['firmware']) ?></td>
            <td><?php echo htmlspecialchars($card['pcie_addr']) ?></td>
            <td><?php echo htmlspecialchars($card['numa_node']) ?></td>
          </tr>
<?php endforeach; ?>
        </tbody>
      </table>
    </div>
  </div>

<?php foreach ($cards as $cardIdx => $card): ?>
  <!-- Card <?php echo $cardIdx ?> -->
  <div class="card mb-3">
    <div class="card-header">
      <i class="material-icons md-18">memory</i>
      <strong>Card <?php echo $cardIdx ?></strong>
      &mdash; <?php echo htmlspecialchars($card['device']) ?>
      @ <?php echo htmlspecialchars($card['pcie_addr']) ?>
      (NUMA <?php echo htmlspecialchars($card['numa_node']) ?>)
      &mdash; FW v<?php echo htmlspecialchars($card['firmware']) ?>
    </div>
    <div class="card-body">

    <!-- VPU Usage Chart -->
<?php
$hasResources = false;
foreach ($resourceSections as $key => $meta) {
  if (!empty($card['summary'][$key])) { $hasResources = true; break; }
}
if ($hasResources):
?>
    <h6><i class="material-icons md-18">speed</i> VPU Usage</h6>
    <div id="chart-card-<?php echo $cardIdx ?>" style="width:100%;height:<?php echo 60 + 40 * count(array_filter($resourceSections, function($k) use ($card) { return !empty($card['summary'][$k]); }, ARRAY_FILTER_USE_KEY)) ?>px;" class="mb-3"></div>
    <div class="table-responsive mb-3">
      <table class="table table-sm table-striped table-hover">
        <thead class="thead-highlight text-left">
          <tr>
            <th>Resource</th>
            <th>Load</th>
            <th>Model Load</th>
            <th>FW Load</th>
            <th>Instances</th>
            <th>Memory</th>
            <th>Critical Mem</th>
            <th>Shared Mem</th>
          </tr>
        </thead>
        <tbody>
<?php foreach ($resourceSections as $key => $meta):
  if (empty($card['summary'][$key])) continue;
  foreach ($card['summary'][$key] as $entry):
?>
          <tr>
            <td><i class="material-icons md-18"><?php echo $meta['icon'] ?></i> <?php echo $meta['label'] ?></td>
            <td><?php echo htmlspecialchars($entry['LOAD'] ?? '0') ?>%</td>
            <td><?php echo htmlspecialchars($entry['MODEL_LOAD'] ?? '0') ?>%</td>
            <td><?php echo htmlspecialchars($entry['FW_LOAD'] ?? '0') ?>%</td>
            <td><?php echo htmlspecialchars($entry['INST'] ?? '0') ?> / <?php echo htmlspecialchars($entry['MAX_INST'] ?? '0') ?></td>
            <td><?php echo htmlspecialchars($entry['MEM'] ?? '0') ?> MB</td>
            <td><?php echo htmlspecialchars($entry['CRITICAL_MEM'] ?? '0') ?> MB</td>
            <td><?php echo htmlspecialchars($entry['SHARE_MEM'] ?? '0') ?> MB</td>
          </tr>
<?php endforeach; endforeach; ?>
        </tbody>
      </table>
    </div>
<?php endif; ?>

    <!-- Infrastructure -->
<?php
$hasInfra = false;
foreach ($infraSections as $key => $meta) {
  if (!empty($card['summary'][$key])) { $hasInfra = true; break; }
}
if ($hasInfra):
?>
    <h6><i class="material-icons md-18">developer_board</i> Infrastructure</h6>
    <div class="table-responsive mb-3">
      <table class="table table-sm table-striped table-hover">
        <thead class="thead-highlight text-left">
          <tr>
            <th>Component</th>
            <th>FW Load</th>
            <th>Shared Mem</th>
            <th>PCIe Throughput</th>
          </tr>
        </thead>
        <tbody>
<?php foreach ($infraSections as $key => $meta):
  if (empty($card['summary'][$key])) continue;
  foreach ($card['summary'][$key] as $entry):
?>
          <tr>
            <td><i class="material-icons md-18"><?php echo $meta['icon'] ?></i> <?php echo $meta['label'] ?></td>
            <td><?php echo htmlspecialchars($entry['FW_LOAD'] ?? '0') ?>%</td>
            <td><?php echo htmlspecialchars($entry['SHARE_MEM'] ?? '0') ?> MB</td>
            <td><?php echo isset($entry['PCIE_THROUGHPUT']) ? htmlspecialchars($entry['PCIE_THROUGHPUT']) : '-' ?></td>
          </tr>
<?php endforeach; endforeach; ?>
        </tbody>
      </table>
    </div>
<?php endif; ?>

    <!-- Active Decoder Sessions -->
<?php if (!empty($card['detailed']['decoder'])): ?>
    <h6><i class="material-icons md-18">input</i> Active Decoder Sessions (<?php echo count($card['detailed']['decoder']) ?>)</h6>
    <div class="table-responsive mb-3">
      <table class="table table-sm table-striped table-hover">
        <thead class="thead-highlight text-left">
          <tr>
            <th>#</th>
            <th>Resolution</th>
            <th>Frame Rate</th>
            <th>Avg Cost</th>
            <th>IDR</th>
            <th>In Frames</th>
            <th>Out Frames</th>
            <th>Session ID</th>
          </tr>
        </thead>
        <tbody>
<?php
$totalDecoderFps = 0;
$totalDecoderInFrames = 0;
$totalDecoderOutFrames = 0;
foreach ($card['detailed']['decoder'] as $decoder):
  $totalDecoderFps += floatval($decoder['FrameRate'] ?? 0);
  $totalDecoderInFrames += intval($decoder['InFrame'] ?? 0);
  $totalDecoderOutFrames += intval($decoder['OutFrame'] ?? 0);
?>
          <tr>
            <td><?php echo htmlspecialchars($decoder['INDEX'] ?? '') ?></td>
            <td><?php echo htmlspecialchars($decoder['Width'] ?? '') ?>x<?php echo htmlspecialchars($decoder['Height'] ?? '') ?></td>
            <td><?php echo htmlspecialchars($decoder['FrameRate'] ?? '') ?> fps</td>
            <td><?php echo htmlspecialchars($decoder['AvgCost'] ?? '') ?></td>
            <td><?php echo htmlspecialchars($decoder['IDR'] ?? '') ?></td>
            <td><?php echo number_format(intval($decoder['InFrame'] ?? 0)) ?></td>
            <td><?php echo number_format(intval($decoder['OutFrame'] ?? 0)) ?></td>
            <td><code><?php echo htmlspecialchars($decoder['SID'] ?? '') ?></code></td>
          </tr>
<?php endforeach; ?>
        </tbody>
        <tfoot class="table-secondary text-left">
          <tr>
            <th colspan="2">Total</th>
            <th><?php echo number_format($totalDecoderFps, 1) ?> fps</th>
            <th></th>
            <th></th>
            <th><?php echo number_format($totalDecoderInFrames) ?></th>
            <th><?php echo number_format($totalDecoderOutFrames) ?></th>
            <th></th>
          </tr>
        </tfoot>
      </table>
    </div>
<?php endif; ?>

    <!-- Active Encoder Sessions -->
<?php if (!empty($card['detailed']['encoder'])): ?>
    <h6><i class="material-icons md-18">output</i> Active Encoder Sessions (<?php echo count($card['detailed']['encoder']) ?>)</h6>
    <div class="table-responsive mb-3">
      <table class="table table-sm table-striped table-hover">
        <thead class="thead-highlight text-left">
          <tr>
            <th>#</th>
            <th>Resolution</th>
            <th>Format</th>
            <th>Frame Rate</th>
            <th>Bitrate</th>
            <th>Avg Bitrate</th>
            <th>Avg Cost</th>
            <th>IDR</th>
            <th>In Frames</th>
            <th>Out Frames</th>
            <th>Session ID</th>
          </tr>
        </thead>
        <tbody>
<?php
$totalEncoderFps = 0;
$totalEncoderBitrate = 0;
$totalEncoderAvgBitrate = 0;
$totalEncoderInFrames = 0;
$totalEncoderOutFrames = 0;
foreach ($card['detailed']['encoder'] as $encoder):
  $totalEncoderFps += floatval($encoder['FrameRate'] ?? 0);
  $totalEncoderBitrate += intval($encoder['BR'] ?? 0);
  $totalEncoderAvgBitrate += intval($encoder['AvgBR'] ?? 0);
  $totalEncoderInFrames += intval($encoder['InFrame'] ?? 0);
  $totalEncoderOutFrames += intval($encoder['OutFrame'] ?? 0);
?>
          <tr>
            <td><?php echo htmlspecialchars($encoder['INDEX'] ?? '') ?></td>
            <td><?php echo htmlspecialchars($encoder['Width'] ?? '') ?>x<?php echo htmlspecialchars($encoder['Height'] ?? '') ?></td>
            <td><?php echo htmlspecialchars($encoder['Format'] ?? '') ?></td>
            <td><?php echo htmlspecialchars($encoder['FrameRate'] ?? '') ?> fps</td>
            <td><?php echo formatBitrate($encoder['BR'] ?? 0) ?></td>
            <td><?php echo formatBitrate($encoder['AvgBR'] ?? 0) ?></td>
            <td><?php echo htmlspecialchars($encoder['AvgCost'] ?? '') ?></td>
            <td><?php echo htmlspecialchars($encoder['IDR'] ?? '') ?></td>
            <td><?php echo number_format(intval($encoder['InFrame'] ?? 0)) ?></td>
            <td><?php echo number_format(intval($encoder['OutFrame'] ?? 0)) ?></td>
            <td><code><?php echo htmlspecialchars($encoder['SID'] ?? '') ?></code></td>
          </tr>
<?php endforeach; ?>
        </tbody>
        <tfoot class="table-secondary text-left">
          <tr>
            <th colspan="3">Total</th>
            <th><?php echo number_format($totalEncoderFps, 1) ?> fps</th>
            <th><?php echo formatBitrate($totalEncoderBitrate) ?></th>
            <th><?php echo formatBitrate($totalEncoderAvgBitrate) ?></th>
            <th></th>
            <th></th>
            <th><?php echo number_format($totalEncoderInFrames) ?></th>
            <th><?php echo number_format($totalEncoderOutFrames) ?></th>
            <th></th>
          </tr>
        </tfoot>
      </table>
    </div>
<?php endif; ?>

    </div><!-- card-body -->
  </div><!-- card -->
<?php endforeach; ?>

<?php endif; ?>
  </div><!-- content -->
</div>
<?php
// Pass chart data to JS
if ($hasData):
  $chartData = [];
  foreach ($cards as $cardIdx => $card) {
    $resources = [];
    foreach ($resourceSections as $key => $meta) {
      if (empty($card['summary'][$key])) continue;
      $entry = $card['summary'][$key][0];
      $totalMem = intval($entry['MEM'] ?? 0) + intval($entry['CRITICAL_MEM'] ?? 0) + intval($entry['SHARE_MEM'] ?? 0);
      $resources[] = [
        'label' => $meta['label'],
        'load' => intval($entry['LOAD'] ?? 0),
        'mem' => $totalMem > 0 ? intval($entry['MEM'] ?? 0) : 0,
      ];
    }
    if (!empty($resources)) {
      $chartData[] = [
        'id' => "chart-card-{$cardIdx}",
        'resources' => $resources,
      ];
    }
  }
?>
<?php echo output_script_if_exists(array('assets/echarts-6.0.0/echarts.min.js'), false) ?>
<script nonce="<?php echo $cspNonce ?>">
var quadraChartData = <?php echo json_encode($chartData) ?>;
</script>
<?php endif; ?>
<?php xhtmlFooter() ?>
