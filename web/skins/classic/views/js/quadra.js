function initCharts() {
  if (typeof echarts === 'undefined' || typeof quadraChartData === 'undefined') return;

  quadraChartData.forEach(function(cardData) {
    var el = document.getElementById(cardData.id);
    if (!el) return;

    var chart = echarts.init(el);

    // Build category labels and data arrays (reversed so first item appears at top)
    var categories = [];
    var loadData = [];
    var memData = [];
    for (var i = cardData.resources.length - 1; i >= 0; i--) {
      var r = cardData.resources[i];
      categories.push(r.label);
      loadData.push(r.load);
      memData.push(r.mem);
    }

    chart.setOption({
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
        data: ['Load', 'Memory Usage'],
        top: 0,
        textStyle: {fontSize: 11}
      },
      grid: {
        left: 70,
        right: 30,
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
          name: 'Load',
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
        },
        {
          name: 'Memory Usage',
          type: 'bar',
          data: memData,
          barWidth: 8,
          itemStyle: {color: '#00bcd4', borderRadius: [0, 3, 3, 0]},
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
      chart.resize();
    });
  });
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
