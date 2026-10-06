// Runs inside the module before main.
Module["preRun"] = Module["preRun"] || [];

// The engine's development hooks are environment variables; on the web they come from the page's
// URL: index.html?HYOSHI_AUTOPLAY=1&HYOSHI_DEBUG_UI=1
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

// The user data folder (settings, imported maps) is SDL's /libsdl/, kept in the browser's
// IndexedDB: loaded here before main starts, and copied back as files change
// (Platform::PumpEvents).
Module["preRun"].push(() => {
  try {
    FS.mkdir("/libsdl");
    FS.mount(IDBFS, {}, "/libsdl");
  } catch (error) {
    console.warn("No browser storage: nothing will be saved.", error);
    return;
  }
  Module["hyoshiStorageMounted"] = true;
  // Ask the browser not to clear it under storage pressure; it may decline.
  navigator.storage?.persist?.();
  addRunDependency("hyoshi-storage");
  FS.syncfs(true, (error) => {
    if (error) {
      console.warn("Could not load saved data:", error);
    }
    removeRunDependency("hyoshi-storage");
  });
});
