// site.js - the website's only script. It makes no network request of its own and stores nothing: it names the
// visitor's system on the download button, adds copy buttons to the commands, plays each film only while it is on
// screen, and closes the phone menu once a section is chosen.
(function () {
  "use strict";

  // 1. Which system is this? Read locally from the browser; nothing is sent anywhere.
  var ua = navigator.userAgent || "";
  var platform = (navigator.userAgentData && navigator.userAgentData.platform) || navigator.platform || "";
  var touchMac = /Mac/i.test(platform) && navigator.maxTouchPoints > 1; // an iPad that says it is a Mac
  var os = "other";
  if (/iPhone|iPad|iPod|Android/i.test(ua) || touchMac) os = "mobile";
  else if (/Mac/i.test(platform) || /Macintosh/i.test(ua)) os = "mac";
  else if (/Win/i.test(platform) || /Windows/i.test(ua)) os = "windows";
  else if (/Linux|X11|CrOS/i.test(platform + " " + ua)) os = "linux";

  var label = { mac: "Download for macOS", linux: "Download for Linux", windows: "Download the source" }[os];
  if (label) {
    document.querySelectorAll("[data-os-label]").forEach(function (a) {
      if (!a.classList.contains("btn-small")) a.textContent = label;
    });
  }
  var mine = document.querySelector('[data-platform="' + os + '"]');
  if (mine) mine.classList.add("is-yours");
  var noteFor = { windows: "[data-windows-note]", mobile: "[data-mobile-note]" }[os];
  var note = noteFor && document.querySelector(noteFor);
  if (note) note.hidden = false;

  // 2. Copy buttons on every command block.
  document.querySelectorAll("pre.code").forEach(function (pre) {
    var code = pre.querySelector("code");
    if (!code) return;
    var button = document.createElement("button");
    button.type = "button";
    button.className = "copy";
    button.textContent = "Copy";
    button.setAttribute("aria-label", "Copy this command");
    button.addEventListener("click", function () {
      var text = code.innerText.trim();
      var done = function (word) {
        button.textContent = word;
        button.classList.add("done");
        setTimeout(function () { button.textContent = "Copy"; button.classList.remove("done"); }, 1600);
      };
      if (navigator.clipboard && window.isSecureContext) {
        navigator.clipboard.writeText(text).then(function () { done("Copied"); }, function () { select(code); done("Selected"); });
      } else {
        select(code);
        done("Selected");
      }
    });
    pre.appendChild(button);
  });
  function select(node) {
    var range = document.createRange();
    range.selectNodeContents(node);
    var sel = window.getSelection();
    sel.removeAllRanges();
    sel.addRange(range);
  }

  // 3. Films: muted videos that play only while on screen, with a poster frame until then, so the page paints fast,
  // a phone downloads only what is seen, and a laptop's battery is spared.
  var reduce = window.matchMedia && window.matchMedia("(prefers-reduced-motion: reduce)").matches;
  var saveData = navigator.connection && navigator.connection.saveData;
  var autoplay = !reduce && !saveData && "IntersectionObserver" in window;
  var films = Array.prototype.slice.call(document.querySelectorAll("video.film"));

  function play(video) {
    video.classList.add("playing");
    var started = video.play();
    if (started && started.catch) started.catch(function () { video.classList.remove("playing"); });
  }
  function stop(video) {
    video.pause();
    video.classList.remove("playing");
  }

  films.forEach(function (video) {
    video.addEventListener("click", function (event) {
      if (!video.paused) {
        if (!video.closest("a")) stop(video);
        return;
      }
      event.preventDefault(); // inside a link, the first tap plays the film and the second follows the link
      play(video);
    });
  });

  if (autoplay) {
    var start = function () {
      var seen = new IntersectionObserver(function (entries) {
        entries.forEach(function (entry) {
          if (entry.isIntersecting && entry.intersectionRatio >= 0.35) play(entry.target);
          else if (!entry.isIntersecting) stop(entry.target);
        });
      }, { threshold: [0, 0.35] });
      films.forEach(function (video) { seen.observe(video); });
    };
    // after the page has painted with its posters, so the films never compete with the first view
    if (document.readyState === "complete") start();
    else window.addEventListener("load", start, { once: true });
  }

  // 4. The phone menu closes once a section is chosen.
  document.querySelectorAll(".menu-panel a").forEach(function (link) {
    link.addEventListener("click", function () {
      var menu = link.closest("details");
      if (menu) menu.open = false;
    });
  });

  // 5. A hairline under the header once the page has scrolled.
  var top = document.querySelector(".top");
  if (top) {
    var onScroll = function () { top.classList.toggle("scrolled", window.scrollY > 8); };
    window.addEventListener("scroll", onScroll, { passive: true });
    onScroll();
  }
})();
