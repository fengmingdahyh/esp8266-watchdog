(function () {
  window.$ = function (id) { return document.getElementById(id); };

  window.esc = function (s) {
    return String(s == null ? '' : s).replace(/[&<>"']/g, function (c) {
      return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c];
    });
  };

  window.req = function (url, opts, cb) {
    var x = new XMLHttpRequest(), done = false;
    x.open((opts && opts.method) || 'GET', url, true);
    x.timeout = (opts && opts.timeout) || 12000;
    if (opts && opts.body) x.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');
    x.onreadystatechange = function () {
      if (x.readyState === 4 && !done) { done = true; cb(x.status === 200 ? x.responseText : null, x.status); }
    };
    x.ontimeout = function () { if (!done) { done = true; cb(null, 0); } };
    x.onerror = function () { if (!done) { done = true; cb(null, -1); } };
    try { x.send(opts && opts.body ? opts.body : null); } catch (e) { cb(null, -2); }
  };

  window.reqJSON = function (url, cb) {
    req(url, {}, function (t) {
      var o = null;
      try { o = JSON.parse(t); } catch (e) { o = null; }
      cb(o);
    });
  };

  window.toast = function (msg) {
    var t = $('toast');
    if (!t) return;
    t.textContent = msg; t.className = 'on';
    clearTimeout(t._h); t._h = setTimeout(function () { t.className = ''; }, 2200);
  };

  window.fmtBytes = function (n) {
    n = n || 0;
    if (n < 1024) return n + ' B';
    if (n < 1048576) return (n / 1024).toFixed(1) + ' KB';
    return (n / 1048576).toFixed(2) + ' MB';
  };

  window.fmtDur = function (s) {
    s = Math.max(0, s | 0);
    var d = (s / 86400) | 0, h = ((s % 86400) / 3600) | 0, m = ((s % 3600) / 60) | 0, sec = s % 60;
    if (d) return d + ' 天 ' + h + ' 时';
    if (h) return h + ' 时 ' + m + ' 分';
    if (m) return m + ' 分 ' + sec + ' 秒';
    return sec + ' 秒';
  };

  window.hhmm = function (epoch) {
    var d = new Date(epoch * 1000);
    return ('0' + d.getHours()).slice(-2) + ':' + ('0' + d.getMinutes()).slice(-2);
  };

  window.ddhhmm = function (epoch) {
    var d = new Date(epoch * 1000);
    return (d.getMonth() + 1) + '/' + d.getDate() + ' ' + ('0' + d.getHours()).slice(-2) + ':' + ('0' + d.getMinutes()).slice(-2);
  };
})();
