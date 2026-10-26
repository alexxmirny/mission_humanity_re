/* Wiki chrome: search box + sortable tables. No dependencies, works from file:// */
(function () {
  "use strict";
  var body = document.body;
  var langRoot = body.getAttribute("data-lang-root") || "";
  var root = body.getAttribute("data-root") || "";
  var noRes = body.getAttribute("data-nores") || "-";

  /* ---- search (index comes from <lang>/search.js, the same data as search.json) ---- */
  var box = document.getElementById("q"), out = document.getElementById("qres");
  var loaded = false, sel = -1;
  function load(cb) {
    if (window.MH_INDEX) { cb(); return; }
    if (loaded) return;
    loaded = true;
    var s = document.createElement("script");
    s.src = langRoot + "search.js";
    s.onload = cb;
    document.head.appendChild(s);
  }
  function esc(t) {
    return String(t).replace(/[&<>"]/g, function (c) {
      return { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c];
    });
  }
  function find(q) {
    q = q.toLowerCase().trim();
    if (!q) return [];
    var hits = [];
    window.MH_INDEX.forEach(function (e) {
      var best = -1;
      e.s.split("|").forEach(function (w) {
        var i = w.indexOf(q);
        if (i === 0) best = Math.max(best, w === q ? 3 : 2);
        else if (i > 0) best = Math.max(best, 1);
      });
      if (best > 0) hits.push([best, e]);
    });
    hits.sort(function (a, b) { return b[0] - a[0] || a[1].n.localeCompare(b[1].n); });
    return hits.slice(0, 25).map(function (h) { return h[1]; });
  }
  function show() {
    var res = find(box.value);
    sel = -1;
    if (!box.value.trim()) { out.hidden = true; return; }
    out.innerHTML = res.length ? res.map(function (e) {
      return '<li><a href="' + langRoot + e.u + '">' +
        (e.i ? '<img src="' + root + "assets/" + e.i + '" alt="">' : "") +
        "<span>" + esc(e.n) + "</span><small>" + esc(e.k) + "</small></a></li>";
    }).join("") : "<li class=\"dim\">" + esc(noRes) + "</li>";
    out.hidden = false;
  }
  if (box) {
    box.addEventListener("focus", function () { load(function () {}); });
    box.addEventListener("input", function () { load(show); });
    box.addEventListener("keydown", function (ev) {
      var items = out.querySelectorAll("a");
      if (ev.key === "ArrowDown" || ev.key === "ArrowUp") {
        ev.preventDefault();
        sel = Math.max(0, Math.min(items.length - 1, sel + (ev.key === "ArrowDown" ? 1 : -1)));
        items.forEach(function (a, i) { a.classList.toggle("sel", i === sel); });
      } else if (ev.key === "Enter" && items.length) {
        window.location.href = items[Math.max(sel, 0)].getAttribute("href");
      } else if (ev.key === "Escape") { out.hidden = true; }
    });
    document.addEventListener("click", function (ev) {
      if (!out.contains(ev.target) && ev.target !== box) out.hidden = true;
    });
  }

  /* ---- sortable tables: cells carry data-v (numeric or lowercase text) ---- */
  document.querySelectorAll("table.sortable").forEach(function (t) {
    var heads = t.tHead.rows[0].cells;
    Array.prototype.forEach.call(heads, function (th, col) {
      th.addEventListener("click", function () {
        var dir = th.classList.contains("asc") ? -1 : 1;
        Array.prototype.forEach.call(heads, function (h) { h.classList.remove("asc", "desc"); });
        th.classList.add(dir === 1 ? "asc" : "desc");
        var rows = Array.prototype.slice.call(t.tBodies[0].rows);
        function key(r) {
          var v = r.cells[col].getAttribute("data-v");
          if (v === null || v === "") return [1, 0, ""];
          var n = parseFloat(v);
          return isNaN(n) ? [0, 0, v] : [0, n, ""];
        }
        rows.sort(function (a, b) {
          var x = key(a), y = key(b);
          if (x[0] !== y[0]) return x[0] - y[0];
          if (x[2] || y[2]) return dir * x[2].localeCompare(y[2]);
          return dir * (x[1] - y[1]);
        });
        rows.forEach(function (r) { t.tBodies[0].appendChild(r); });
      });
    });
  });
})();
