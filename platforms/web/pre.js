// Runs inside the module before main. The engine's development hooks are environment variables;
// on the web they come from the page's URL: index.html?HYOSHI_AUTOPLAY=1&HYOSHI_DEBUG_UI=1
Module["preRun"] = Module["preRun"] || [];
Module["preRun"].push(() => {
  if (typeof location === "undefined") {
    return;
  }
  for (const [key, value] of new URLSearchParams(location.search)) {
    if (key.startsWith("HYOSHI_")) {
      ENV[key] = value;
    }
  }
});
