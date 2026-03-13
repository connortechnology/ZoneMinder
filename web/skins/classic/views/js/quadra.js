function initCharts() {
  if (typeof echarts === 'undefined') return;

  // Combined VPU Usage chart (all cards)
  if (typeof quadraVpuData !== 'undefined' && quadraVpuData.length > 0) {
    var el = document.getElementById('chart-vpu-usage');
    if (el) {
      var vpuChart = echarts.init(el);

      var categories = [];
      var loadData = [];
      var memData = [];
      for (var i = quadraVpuData.length - 1; i >= 0; i--) {
        var r = quadraVpuData[i];
        categories.push(r.label);
        loadData.push(r.load);
        memData.push(r.mem);
      }

      vpuChart.setOption({
        tooltip: {
          trigger: 'axis',
          axisPointer: {type: 'shadow'},
          formatter: function(params) {
            var label = params[0].name;
            var lines = [label];
            params.forEach(function(p) {
              lines.push(p.marker + ' ' + p.seriesName + ': ' + p.value + '%');
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
          left: (typeof quadraMultiCard !== 'undefined' && quadraMultiCard) ? 120 : 80,
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
            label: {
              show: true,
              position: 'right',
              fontSize: 10,
              formatter: '{c}%'
            }
          },
          {
            name: 'Load Average',
            type: 'bar',
            data: loadData,
            barWidth: 8,
            itemStyle: {color: '#8e8e8e', borderRadius: [0, 3, 3, 0]},
            label: {
              show: true,
              position: 'right',
              fontSize: 10,
              formatter: '{c}%'
            }
          }
        ]
      });

      window.addEventListener('resize', function() {
        vpuChart.resize();
      });
    }
  }

  // Host Usage chart
  if (typeof quadraHostUsage !== 'undefined' && quadraHostUsage.length > 0) {
    var hostEl = document.getElementById('chart-host-usage');
    if (hostEl) {
      var hostChart = echarts.init(hostEl);

      var hostCategories = [];
      var hostValues = [];
      var infoLabels = [];
      for (var i = quadraHostUsage.length - 1; i >= 0; i--) {
        var h = quadraHostUsage[i];
        hostCategories.push(h.label);
        hostValues.push(h.value);
        infoLabels.push(h.info);
      }

      hostChart.setOption({
        tooltip: {
          trigger: 'axis',
          axisPointer: {type: 'shadow'},
          formatter: function(params) {
            var p = params[0];
            var idx = p.dataIndex;
            return p.name + ': ' + p.value + '% (' + infoLabels[idx] + ')';
          }
        },
        grid: {
          left: 80,
          right: 80,
          top: 10,
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
          data: hostCategories,
          axisLabel: {fontSize: 12, fontWeight: 'bold'},
          axisTick: {show: false}
        },
        series: [
          {
            type: 'bar',
            data: hostValues.map(function(v) {
              return {
                value: v,
                label: {
                  show: true,
                  position: v > 15 ? 'insideLeft' : 'right',
                  fontSize: 10,
                  color: v > 15 ? '#fff' : '#333',
                  formatter: v + '%'
                }
              };
            }),
            barWidth: 12,
            itemStyle: {color: '#38c5eb', borderRadius: [0, 3, 3, 0]}
          }
        ]
      });

      // Add right-aligned info text using graphic elements
      var graphicElements = [];
      for (var gi = 0; gi < infoLabels.length; gi++) {
        graphicElements.push({
          type: 'text',
          right: 10,
          top: hostChart.convertToPixel('grid', [0, gi])[1] - 7,
          style: {
            text: infoLabels[gi],
            fontSize: 11,
            fill: '#666',
            textAlign: 'right'
          },
          silent: true
        });
      }
      hostChart.setOption({graphic: graphicElements});

      window.addEventListener('resize', function() {
        hostChart.resize();
        var elems = [];
        for (var ri = 0; ri < infoLabels.length; ri++) {
          elems.push({
            type: 'text',
            right: 10,
            top: hostChart.convertToPixel('grid', [0, ri])[1] - 7,
            style: {
              text: infoLabels[ri],
              fontSize: 11,
              fill: '#666',
              textAlign: 'right'
            },
            silent: true
          });
        }
        hostChart.setOption({graphic: elems});
      });
    }
  }
}

function initPage() {
  // Manage the BACK button
  document.getElementById("backBtn").addEventListener("click", function onBackClick(evt) {
    evt.preventDefault();
    window.history.back();
  });

  // Disable the back button if there is nothing to go back to
  $j('#backBtn').prop('disabled', !document.referrer.length);

  // Manage the REFRESH Button
  document.getElementById("refreshBtn").addEventListener("click", function onRefreshClick(evt) {
    evt.preventDefault();
    window.location.reload(true);
  });

  // Initialize ECharts
  initCharts();

  // Auto-refresh page
  if (quadraRefreshInterval > 0) {
    setInterval(function() {
      window.location.reload(true);
    }, quadraRefreshInterval);
  }
}

$j(document).ready(initPage);
