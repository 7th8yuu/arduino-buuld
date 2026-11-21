// Chen vao dau trang: chuyen moi fetch('/api/...') cua giao dien web thanh lenh TCP qua cau noi native (LedNative).
(function () {
  var pend = {}, seq = 0, realFetch = window.fetch;
  window.__ledResp = function (id, ok, body) {
    var p = pend[id];
    if (!p) return;
    delete pend[id];
    if (ok) p.res(new Response(body, { status: 200, headers: { 'Content-Type': 'application/json' } }));
    else p.rej(new TypeError('Failed to fetch'));
  };
  window.fetch = function (url, opt) {
    var u = typeof url === 'string' ? url : (url && url.url) || '';
    if (u.indexOf('/api/') !== 0) return realFetch.apply(window, arguments);
    opt = opt || {};
    return new Promise(function (res, rej) {
      var id = ++seq;
      pend[id] = { res: res, rej: rej };
      var sig = opt.signal;
      if (sig) {
        var abort = function () {
          if (!pend[id]) return;
          delete pend[id];
          var e = new Error('Aborted');
          e.name = 'AbortError';
          rej(e);
        };
        if (sig.aborted) { abort(); return; }
        sig.addEventListener('abort', abort);
      }
      try {
        LedNative.req(id, u, opt.body == null ? '' : String(opt.body));
      } catch (e) {
        delete pend[id];
        rej(new TypeError('Failed to fetch'));
      }
    });
  };
})();
