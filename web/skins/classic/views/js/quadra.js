// Chart instances (persistent across refreshes)
let vpuChart = null;
let hostChart = null;
let hostInfoLabels = [];
let ajaxPending = false;

function escHtml(str) {
  if (str === null || str === undefined) return '';
  const d = document.createElement('div');
  d.appendChild(document.createTextNode(String(str)));
  return d.innerHTML;
}

function numberFormat(n, decimals = 0) {
  return Number(n).toLocaleString(undefined, {
    minimumFractionDigits: decimals,
    maximumFractionDigits: decimals
  });
}

// ---- Chart rendering ----

function renderVpuChart(vpuData, multiCard) {
  const el = document.getElementById('chart-vpu-usage');
  if (!el || typeof echarts === 'undefined') return;

  if (!vpuData || !vpuData.length) {
    el.style.display = 'none';
    return;
  }
  el.style.display = '';

  // Resize height based on row count
  el.style.height = Math.max(150, 50 + 35 * vpuData.length) + 'px';

  if (!vpuChart) {
    vpuChart = echarts.init(el);
    window.addEventListener('resize', () => vpuChart.resize());
  } else {
    vpuChart.resize();
  }

  const categories = [];
  const loadData = [];
  const memData = [];
  for (let i = vpuData.length - 1; i >= 0; i--) {
    const r = vpuData[i];
    categories.push(r.label);
    loadData.push(r.load);
    memData.push(r.mem);
  }

  vpuChart.setOption({
    tooltip: {
      trigger: 'axis',
      axisPointer: {type: 'shadow'},
      formatter: (params) => {
        const lines = [params[0].name];
        params.forEach((p) => {
          lines.push(`${p.marker} ${p.seriesName}: ${p.value}%`);
        });
        return lines.join('<br/>');
      }
    },
    legend: {
      data: ['Memory Usage', 'Load Average'],
      top: 0,
      textStyle: {fontSize: 11}
    },
    grid: {
      left: multiCard ? 120 : 80,
      right: 40,
      top: 30,
      bottom: 25
    },
    xAxis: {
      type: 'value',
      min: 0,
      max: 100,
      axisLabel: {formatter: '{value}%', fontSize: 10},
      splitLine: {lineStyle: {type: 'dashed', color: '#e0e0e0'}}
    },
    yAxis: {
      type: 'category',
      data: categories,
      axisLabel: {fontSize: 11},
      axisTick: {show: false}
    },
    series: [
      {
        name: 'Memory Usage',
        type: 'bar',
        data: memData,
        barWidth: 8,
        itemStyle: {color: '#38c5eb', borderRadius: [0, 3, 3, 0]},
        label: {show: true, position: 'right', fontSize: 10, formatter: '{c}%'}
      },
      {
        name: 'Load Average',
        type: 'bar',
        data: loadData,
        barWidth: 8,
        itemStyle: {color: '#8e8e8e', borderRadius: [0, 3, 3, 0]},
        label: {show: true, position: 'right', fontSize: 10, formatter: '{c}%'}
      }
    ]
  });
}

function renderHostChart(hostData) {
  const el = document.getElementById('chart-host-usage');
  if (!el || typeof echarts === 'undefined') return;

  if (!hostData || !hostData.length) {
    el.style.display = 'none';
    return;
  }
  el.style.display = '';

  el.style.height = Math.max(150, 50 + 35 * hostData.length) + 'px';

  if (!hostChart) {
    hostChart = echarts.init(el);
    window.addEventListener('resize', () => {
      hostChart.resize();
      positionHostInfoLabels();
    });
  } else {
    hostChart.resize();
  }

  const categories = [];
  const values = [];
  hostInfoLabels = [];
  for (let i = hostData.length - 1; i >= 0; i--) {
    const h = hostData[i];
    categories.push(h.label);
    values.push(h.value);
    hostInfoLabels.push(h.info);
  }

  hostChart.setOption({
    tooltip: {
      trigger: 'axis',
      axisPointer: {type: 'shadow'},
      formatter: (params) => {
        const p = params[0];
        return `${p.name}: ${p.value}% (${hostInfoLabels[p.dataIndex]})`;
      }
    },
    grid: {left: 80, right: 80, top: 10, bottom: 25},
    xAxis: {
      type: 'value',
      min: 0,
      max: 100,
      axisLabel: {formatter: '{value}%', fontSize: 10},
      splitLine: {lineStyle: {type: 'dashed', color: '#e0e0e0'}}
    },
    yAxis: {
      type: 'category',
      data: categories,
      axisLabel: {fontSize: 12, fontWeight: 'bold'},
      axisTick: {show: false}
    },
    series: [{
      type: 'bar',
      data: values.map((v) => ({
        value: v,
        label: {
          show: true,
          position: v > 15 ? 'insideLeft' : 'right',
          fontSize: 10,
          color: v > 15 ? '#fff' : '#333',
          formatter: `${v}%`
        }
      })),
      barWidth: 12,
      itemStyle: {color: '#38c5eb', borderRadius: [0, 3, 3, 0]}
    }]
  });

  positionHostInfoLabels();
}

function positionHostInfoLabels() {
  if (!hostChart || !hostInfoLabels.length) return;
  const elems = [];
  for (let i = 0; i < hostInfoLabels.length; i++) {
    const pos = hostChart.convertToPixel('grid', [0, i]);
    if (!pos) continue;
    elems.push({
      type: 'text',
      right: 10,
      top: pos[1] - 7,
      style: {text: hostInfoLabels[i], fontSize: 11, fill: '#666', textAlign: 'right'},
      silent: true
    });
  }
  hostChart.setOption({graphic: elems});
}

// ---- Table rendering ----

function renderVpuDevices(cards) {
  const cardEl = document.getElementById('vpuDevicesCard');
  const tbody = document.getElementById('vpuDevicesBody');
  if (!cards || !Object.keys(cards).length) {
    cardEl.style.display = 'none';
    return;
  }
  cardEl.style.display = '';
  const rows = Object.keys(cards).sort().map((idx) => {
    const c = cards[idx];
    return '<tr>' +
      `<td>${escHtml(c.device)}</td>` +
      `<td>${escHtml(c.firmware)}</td>` +
      `<td>${escHtml(c.pcie_addr)}</td>` +
      `<td>${escHtml(c.numa_node)}</td>` +
      '</tr>';
  });
  tbody.innerHTML = rows.join('');
}

function renderCardDetails(cards) {
  const container = document.getElementById('cardDetails');
  if (!cards || !Object.keys(cards).length) {
    container.innerHTML = '';
    return;
  }

  const sections = Object.keys(cards).sort().map((idx) => {
    const c = cards[idx];
    let html = '<div class="card mb-3">';
    html += '<div class="card-header">';
    html += '<i class="material-icons md-18">memory</i> ';
    html += `<strong>Card ${escHtml(idx)}</strong>`;
    html += ` &mdash; ${escHtml(c.device)}`;
    html += ` @ ${escHtml(c.pcie_addr)}`;
    html += ` (NUMA ${escHtml(c.numa_node)})`;
    html += ` &mdash; FW v${escHtml(c.firmware)}`;
    html += '</div><div class="card-body">';

    // Resources table
    if (c.resources && c.resources.length) {
      html += '<h6><i class="material-icons md-18">speed</i> Resources</h6>';
      html += '<div class="table-responsive mb-3"><table class="table table-sm table-striped table-hover">';
      html += '<thead class="thead-highlight text-left"><tr>';
      html += '<th>Resource</th><th>Load</th><th>Model Load</th><th>FW Load</th>';
      html += '<th>Instances</th><th>Memory</th><th>Critical Mem</th><th>Shared Mem</th>';
      html += '</tr></thead><tbody>';
      for (const r of c.resources) {
        html += '<tr>';
        html += `<td><i class="material-icons md-18">${escHtml(r.icon)}</i> ${escHtml(r.label)}</td>`;
        html += `<td>${escHtml(r.load)}%</td>`;
        html += `<td>${escHtml(r.model_load)}%</td>`;
        html += `<td>${escHtml(r.fw_load)}%</td>`;
        html += `<td>${escHtml(r.inst)} / ${escHtml(r.max_inst)}</td>`;
        html += `<td>${escHtml(r.mem)} MB</td>`;
        html += `<td>${escHtml(r.critical_mem)} MB</td>`;
        html += `<td>${escHtml(r.share_mem)} MB</td>`;
        html += '</tr>';
      }
      html += '</tbody></table></div>';
    }

    // Infrastructure table
    if (c.infra && c.infra.length) {
      html += '<h6><i class="material-icons md-18">developer_board</i> Infrastructure</h6>';
      html += '<div class="table-responsive mb-3"><table class="table table-sm table-striped table-hover">';
      html += '<thead class="thead-highlight text-left"><tr>';
      html += '<th>Component</th><th>FW Load</th><th>Shared Mem</th><th>PCIe Throughput</th>';
      html += '</tr></thead><tbody>';
      for (const inf of c.infra) {
        html += '<tr>';
        html += `<td><i class="material-icons md-18">${escHtml(inf.icon)}</i> ${escHtml(inf.label)}</td>`;
        html += `<td>${escHtml(inf.fw_load)}%</td>`;
        html += `<td>${escHtml(inf.share_mem)} MB</td>`;
        html += `<td>${inf.pcie_throughput !== null ? escHtml(inf.pcie_throughput) : '-'}</td>`;
        html += '</tr>';
      }
      html += '</tbody></table></div>';
    }

    // Decoder sessions
    if (c.decoders && c.decoders.length) {
      let totalFps = 0; let totalIn = 0; let totalOut = 0;
      for (const d of c.decoders) {
        totalFps += parseFloat(d.fps) || 0;
        totalIn += d.in_frames;
        totalOut += d.out_frames;
      }
      html += `<h6><i class="material-icons md-18">input</i> Active Decoder Sessions (${c.decoders.length})</h6>`;
      html += '<div class="table-responsive mb-3"><table class="table table-sm table-striped table-hover">';
      html += '<thead class="thead-highlight text-left"><tr>';
      html += '<th>#</th><th>Resolution</th><th>Frame Rate</th><th>Avg Cost</th>';
      html += '<th>IDR</th><th>In Frames</th><th>Out Frames</th><th>Session ID</th>';
      html += '</tr></thead><tbody>';
      for (const d of c.decoders) {
        html += '<tr>';
        html += `<td>${escHtml(d.index)}</td>`;
        html += `<td>${escHtml(d.width)}x${escHtml(d.height)}</td>`;
        html += `<td>${escHtml(d.fps)} fps</td>`;
        html += `<td>${escHtml(d.avg_cost)}</td>`;
        html += `<td>${escHtml(d.idr)}</td>`;
        html += `<td>${numberFormat(d.in_frames)}</td>`;
        html += `<td>${numberFormat(d.out_frames)}</td>`;
        html += `<td><code>${escHtml(d.sid)}</code></td>`;
        html += '</tr>';
      }
      html += '</tbody><tfoot class="table-secondary text-left"><tr>';
      html += '<th colspan="2">Total</th>';
      html += `<th>${numberFormat(totalFps, 1)} fps</th>`;
      html += '<th></th><th></th>';
      html += `<th>${numberFormat(totalIn)}</th>`;
      html += `<th>${numberFormat(totalOut)}</th>`;
      html += '<th></th></tr></tfoot></table></div>';
    }

    // Encoder sessions
    if (c.encoders && c.encoders.length) {
      let eTotalFps = 0; let eTotalIn = 0; let eTotalOut = 0;
      for (const e of c.encoders) {
        eTotalFps += parseFloat(e.fps) || 0;
        eTotalIn += e.in_frames;
        eTotalOut += e.out_frames;
      }
      html += `<h6><i class="material-icons md-18">output</i> Active Encoder Sessions (${c.encoders.length})</h6>`;
      html += '<div class="table-responsive mb-3"><table class="table table-sm table-striped table-hover">';
      html += '<thead class="thead-highlight text-left"><tr>';
      html += '<th>#</th><th>Resolution</th><th>Format</th><th>Frame Rate</th>';
      html += '<th>Bitrate</th><th>Avg Bitrate</th><th>Avg Cost</th>';
      html += '<th>IDR</th><th>In Frames</th><th>Out Frames</th><th>Session ID</th>';
      html += '</tr></thead><tbody>';
      for (const e of c.encoders) {
        html += '<tr>';
        html += `<td>${escHtml(e.index)}</td>`;
        html += `<td>${escHtml(e.width)}x${escHtml(e.height)}</td>`;
        html += `<td>${escHtml(e.format)}</td>`;
        html += `<td>${escHtml(e.fps)} fps</td>`;
        html += `<td>${escHtml(e.bitrate)}</td>`;
        html += `<td>${escHtml(e.avg_bitrate)}</td>`;
        html += `<td>${escHtml(e.avg_cost)}</td>`;
        html += `<td>${escHtml(e.idr)}</td>`;
        html += `<td>${numberFormat(e.in_frames)}</td>`;
        html += `<td>${numberFormat(e.out_frames)}</td>`;
        html += `<td><code>${escHtml(e.sid)}</code></td>`;
        html += '</tr>';
      }
      html += '</tbody><tfoot class="table-secondary text-left"><tr>';
      html += '<th colspan="3">Total</th>';
      html += `<th>${numberFormat(eTotalFps, 1)} fps</th>`;
      html += '<th></th><th></th><th></th><th></th>';
      html += `<th>${numberFormat(eTotalIn)}</th>`;
      html += `<th>${numberFormat(eTotalOut)}</th>`;
      html += '<th></th></tr></tfoot></table></div>';
    }

    html += '</div></div>';
    return html;
  });

  container.innerHTML = sections.join('');
}

function showAlert(message, type) {
  const el = document.getElementById('quadraAlert');
  el.style.display = '';
  el.innerHTML = `<div class="alert alert-${type}">${message}</div>`;
}

function hideAlert() {
  const el = document.getElementById('quadraAlert');
  el.style.display = 'none';
  el.innerHTML = '';
}

// ---- Apply full data update ----

function applyData(data) {
  hideAlert();

  // Timestamp
  const tsEl = document.getElementById('vpuTimestamp');
  if (tsEl) {
    let ts = data.timestamp ? escHtml(data.timestamp) : '';
    if (data.uptime) ts += ` &mdash; up ${escHtml(data.uptime)}`;
    tsEl.innerHTML = ts;
  }

  // Charts
  renderVpuChart(data.vpuChartData, data.multiCard);
  renderHostChart(data.hostUsage);

  // Tables
  renderVpuDevices(data.cards);
  renderCardDetails(data.cards);
}

// ---- AJAX refresh ----

function refreshData() {
  if (ajaxPending) return;
  ajaxPending = true;

  $j.ajax({
    url: `${thisUrl}?request=quadra`,
    method: 'GET',
    dataType: 'json',
    timeout: 30000,
    success: (resp) => {
      ajaxPending = false;
      if (resp.result === 'Error') {
        showAlert(`<strong>Error:</strong> ${escHtml(resp.message)}`, 'danger');
        return;
      }
      applyData(resp);
    },
    error: (jqXHR, textStatus) => {
      ajaxPending = false;
      if (textStatus !== 'abort') {
        console.log('Quadra AJAX error:', textStatus);
      }
    }
  });
}

// ---- Init ----

function initPage() {
  // Back button
  document.getElementById('backBtn').addEventListener('click', (evt) => {
    evt.preventDefault();
    window.history.back();
  });
  $j('#backBtn').prop('disabled', !document.referrer.length);

  // Refresh button triggers immediate AJAX refresh
  document.getElementById('refreshBtn').addEventListener('click', (evt) => {
    evt.preventDefault();
    refreshData();
  });

  // Apply initial server-rendered data
  if (typeof quadraInitialError === 'string' && quadraInitialError) {
    showAlert(`<strong>Error running ni_rsrc_mon:</strong><br>${escHtml(quadraInitialError)}`, 'danger');
  } else if (typeof quadraInitialData !== 'undefined' && quadraInitialData) {
    applyData(quadraInitialData);
  } else {
    showAlert('<strong>No data:</strong> ni_rsrc_mon returned no resource data. ' +
      'Check that a NetInt Quadra device is installed and that the web server user has permission to access it.', 'warning');
  }

  // Auto-refresh via AJAX instead of full page reload
  if (quadraRefreshInterval > 0) {
    setInterval(refreshData, quadraRefreshInterval);
  }
}

$j(document).ready(initPage);
