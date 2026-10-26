/* Tech tree interaction: select = highlight prerequisite chain + unlocks,
   hover = stat card, double-click / "Open page" = navigate. No dependencies. */
(function () {
  "use strict";

  function init(root) {
    var svg = root.querySelector(".tt-svg");
    var scroll = root.querySelector(".tt-scroll");
    var info = root.querySelector(".tt-info");
    var card = root.querySelector(".tt-card");
    var D = JSON.parse(root.querySelector(".tt-data").textContent);
    var UI = JSON.parse(root.querySelector(".tt-ui").textContent);
    if (!svg) return;

    var succ = {};
    Object.keys(D).forEach(function (id) { succ[id] = []; });
    Object.keys(D).forEach(function (id) {
      D[id].d.forEach(function (p) { if (succ[p]) succ[p].push(id); });
    });
    Object.keys(succ).forEach(function (id) { succ[id].sort(); });

    var nodeEl = {};
    svg.querySelectorAll(".tt-node").forEach(function (n) { nodeEl[n.getAttribute("data-id")] = n; });
    var edgeEls = Array.prototype.slice.call(svg.querySelectorAll(".tt-edge"));

    function closure(start, next) {
      var seen = {}, stack = [start];
      while (stack.length) {
        var cur = stack.pop();
        (next[cur] || []).forEach(function (n) {
          if (!seen[n]) { seen[n] = true; stack.push(n); }
        });
      }
      return seen;
    }
    var pre = {}, selected = null;
    Object.keys(D).forEach(function (id) { pre[id] = D[id].d; });

    function esc(s) {
      return String(s).replace(/[&<>"]/g, function (c) {
        return { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c];
      });
    }

    function links(ids) {
      if (!ids.length) return esc(UI.none || "-");
      return ids.map(function (i) {
        return '<button type="button" class="lnk" data-go="' + esc(i) + '">' + esc(D[i].l) + "</button>";
      }).join(", ");
    }

    function clear() {
      selected = null;
      svg.classList.remove("has-sel");
      Object.keys(nodeEl).forEach(function (id) { nodeEl[id].classList.remove("sel", "pre", "unl"); });
      edgeEls.forEach(function (e) { e.classList.remove("on-pre", "on-unl"); });
      info.innerHTML = "";
    }

    function select(id) {
      if (!D[id]) return;
      if (selected === id) { clear(); return; }
      clear();
      selected = id;
      var anc = closure(id, pre), desc = closure(id, succ);
      svg.classList.add("has-sel");
      Object.keys(nodeEl).forEach(function (n) {
        var c = nodeEl[n].classList;
        if (n === id) c.add("sel");
        else if (anc[n]) c.add("pre");
        else if (desc[n]) c.add("unl");
      });
      edgeEls.forEach(function (e) {
        var a = e.getAttribute("data-from"), b = e.getAttribute("data-to");
        if ((anc[a] || a === id) && (anc[b] || b === id)) e.classList.add("on-pre");
        else if ((desc[b] || b === id) && (desc[a] || a === id)) e.classList.add("on-unl");
      });
      var d = D[id];
      var h = "<b>" + esc(d.l) + "</b>" + (d.s ? " <i>(" + esc(UI.start || "start") + ")</i>" : "");
      if (d.h) h += ' - <a href="' + esc(d.h) + '">' + esc(UI.open || "Open") + "</a>";
      h += '<div class="tt-list">' + esc(UI.requires || "Requires") + ": " + links(d.d) + "</div>";
      h += '<div class="tt-list">' + esc(UI.unlocks || "Unlocks") + ": " + links(succ[id]) + "</div>";
      info.innerHTML = h;
      var el = nodeEl[id];
      if (el && el.scrollIntoView) {
        try { el.scrollIntoView({ block: "nearest", inline: "nearest" }); } catch (e) { /* old browsers */ }
      }
    }

    function showCard(id, x, y) {
      var d = D[id];
      if (!d) return;
      var h = "<b>" + esc(d.l) + "</b>";
      h += esc(d.k) + (d.o ? " - " + esc(d.o) : "");
      h += "<dl>";
      d.c.forEach(function (r) { h += "<dt>" + esc(r[0]) + "</dt><dd>" + esc(r[1]) + "</dd>"; });
      h += "<dt>" + esc(UI.requires || "Requires") + "</dt><dd>" + d.d.length + "</dd>";
      h += "<dt>" + esc(UI.unlocks || "Unlocks") + "</dt><dd>" + (succ[id] || []).length + "</dd></dl>";
      card.innerHTML = h;
      card.hidden = false;
      var w = card.offsetWidth, ht = card.offsetHeight;
      var left = Math.min(x + 14, window.innerWidth - w - 8);
      var top = y + 14 + ht > window.innerHeight ? y - ht - 8 : y + 14;
      card.style.left = Math.max(4, left) + "px";
      card.style.top = Math.max(4, top) + "px";
    }
    function hideCard() { card.hidden = true; }

    function nodeOf(t) { return t.closest ? t.closest(".tt-node") : null; }

    svg.addEventListener("click", function (ev) {
      var n = nodeOf(ev.target);
      if (n) select(n.getAttribute("data-id")); else clear();
    });
    svg.addEventListener("dblclick", function (ev) {
      var n = nodeOf(ev.target);
      if (n && D[n.getAttribute("data-id")].h) window.location.href = D[n.getAttribute("data-id")].h;
    });
    svg.addEventListener("mouseover", function (ev) {
      var n = nodeOf(ev.target);
      if (n) showCard(n.getAttribute("data-id"), ev.clientX, ev.clientY);
    });
    svg.addEventListener("mousemove", function (ev) {
      var n = nodeOf(ev.target);
      if (n) showCard(n.getAttribute("data-id"), ev.clientX, ev.clientY); else hideCard();
    });
    svg.addEventListener("mouseleave", hideCard);
    svg.addEventListener("focusin", function (ev) {
      var n = nodeOf(ev.target);
      if (!n) return;
      var r = n.getBoundingClientRect();
      showCard(n.getAttribute("data-id"), r.right, r.top);
    });
    svg.addEventListener("focusout", hideCard);
    svg.addEventListener("keydown", function (ev) {
      var n = nodeOf(ev.target);
      if (n && (ev.key === "Enter" || ev.key === " ")) {
        ev.preventDefault();
        select(n.getAttribute("data-id"));
      } else if (ev.key === "Escape") {
        clear();
      }
    });
    info.addEventListener("click", function (ev) {
      var go = ev.target.getAttribute && ev.target.getAttribute("data-go");
      if (go) { clear(); select(go); }
    });

    var vb = svg.viewBox.baseVal, scale = 1;
    function setScale(s) {
      scale = Math.max(0.2, Math.min(2.5, s));
      svg.setAttribute("width", Math.round(vb.width * scale));
      svg.setAttribute("height", Math.round(vb.height * scale));
    }
    root.querySelectorAll("[data-zoom]").forEach(function (b) {
      b.addEventListener("click", function () {
        var m = b.getAttribute("data-zoom");
        if (m === "in") setScale(scale * 1.25);
        else if (m === "out") setScale(scale / 1.25);
        else setScale(Math.min(1, (scroll.clientWidth - 2) / vb.width));
      });
    });
  }

  function boot() {
    document.querySelectorAll(".tt-wrap").forEach(init);
  }
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", boot);
  else boot();
})();
