// FilterTrack Web Bluetooth bridge.
// Drop-in replacement for the Android WebAppInterface, using the browser's
// Web Bluetooth API instead of a native BLE stack. Talks to index.html and
// csv-analysis.html exactly the way the Android app's WebView bridge did:
// JS calls window.Android.<method>(...) to act, and this file calls
// window.FilterTrackBridge.<method>(...) to push events back into the page.
//
// Requires a browser with Web Bluetooth support. Safari on iOS has none —
// use the Bluefy app there. Chrome/Edge on Android and desktop work directly.

(() => {
  const SERVICE_UUID = "000000ff-0000-1000-8000-00805f9b34fb";
  const CHAR_CMD_NOTIFY_UUID = "0000ff01-0000-1000-8000-00805f9b34fb";
  const CHAR_OTA_UUID = "0000ff02-0000-1000-8000-00805f9b34fb";
  const CCCD_UUID = "00002902-0000-1000-8000-00805f9b34fb";
  const BI_URL = "https://filtertrack-api.fly.dev/bi";
  // BI "user" (read-only) access key, handed to the dashboard in the URL
  // fragment (#key=...) so it signs in on open. The fragment never reaches
  // the server. Anyone who can open this web app can read this key.
  // Set by bi-config.js (deployed with the site, kept out of git).
  const BI_USER_KEY = window.FILTERTRACK_BI_USER_KEY || "";
  const biLink = (query = "") =>
    `${BI_URL}${query}${BI_USER_KEY ? `#key=${encodeURIComponent(BI_USER_KEY)}` : ""}`;
  const OTA_MAX_BYTES = 4 * 1024 * 1024;
  const OTA_CHUNK_BYTES = 180;

  const knownDevices = new Map(); // address (device.id) -> BluetoothDevice

  let activeDevice = null;
  let gattServer = null;
  let cmdChar = null;
  let otaChar = null;
  let otaCancelled = false;

  // Surfaces bridge-internal status directly in the page's own "Raw ESP"
  // debug panel (via onDataReceived) — the only way to see what's happening
  // on phones/browsers where the JS console isn't reachable (e.g. Bluefy).
  // The page clears that panel when it receives "Connected", so lines logged
  // during the connect flow are kept and replayed right after it.
  const connectDiag = [];
  function diag(msg) {
    connectDiag.push(msg);
    remoteLog(msg);
    push("onDataReceived", `BRIDGE:${msg}`);
  }

  // Sends a diagnostic line to the server's /log endpoint, which only
  // records it in the access log. Lets the bridge be debugged on an iPhone,
  // where Bluefy's JS console can't be reached.
  const sessionTag = Math.random().toString(36).slice(2, 8);
  function remoteLog(msg) {
    try {
      fetch(`log?s=${sessionTag}&t=${Date.now()}&m=${encodeURIComponent(msg)}`, {
        cache: "no-store",
        keepalive: true,
      }).catch(() => {});
    } catch {}
  }
  remoteLog(`bridge v10 loaded; bluetooth=${"bluetooth" in navigator}; ua=${navigator.userAgent}`);

  // Offline shell (sw.js). Not every WebBLE browser allows service workers —
  // Bluefy runs on WKWebView, where they need the host app's opt-in — so
  // report what happened; the page works the same online either way.
  if ("serviceWorker" in navigator) {
    navigator.serviceWorker
      .register("sw.js")
      .then((reg) => remoteLog(`service worker registered (scope ${reg.scope})`))
      .catch((e) => remoteLog(`service worker FAILED: ${e.message || e}`));
  } else {
    remoteLog("service worker unsupported in this browser");
  }
  if (navigator.storage && navigator.storage.persist) {
    navigator.storage.persist()
      .then((granted) => remoteLog(`persistent storage: ${granted}`))
      .catch(() => {});
  }

  function push(method, ...args) {
    if (method !== "onDataReceived" && method !== "onOtaProgress") {
      remoteLog(`${method}(${args.map(String).join(", ")})`);
    }
    try {
      if (window.FilterTrackBridge && typeof window.FilterTrackBridge[method] === "function") {
        window.FilterTrackBridge[method](...args);
      }
    } catch (e) {
      console.error("[bridge] push failed:", method, e);
    }
  }

  function bleUnsupported() {
    return !("bluetooth" in navigator);
  }

  let notifyCount = 0;
  function handleNotify(event) {
    const value = event.target.value;
    const text = new TextDecoder("utf-8").decode(value);
    notifyCount += 1;
    if (notifyCount <= 5 || notifyCount % 50 === 0) remoteLog(`notify #${notifyCount}: ${text}`);
    push("onDataReceived", text);

    // Mirror device-reported OTA state into the dedicated OTA callbacks too,
    // in case the page's OTA panel listens there instead of onDataReceived.
    if (text.startsWith("OTA=OK")) {
      push("onOtaStatus", "success", "Atualização concluída");
    } else if (text.startsWith("OTA=ERRO")) {
      push("onOtaStatus", "error", text.slice(text.indexOf("=") + 1));
    } else if (text.startsWith("OTA=ABORTED")) {
      push("onOtaStatus", "idle", "Atualização cancelada pelo dispositivo");
    }
  }

  let lastNotifyEvent = null;
  function handleNotifyOnce(event) {
    if (event === lastNotifyEvent) return;
    lastNotifyEvent = event;
    handleNotify(event);
  }

  function onGattDisconnected() {
    ready = false;
    cmdChar = null;
    otaChar = null;
    gattServer = null;
    push("onConnectionStateChanged", "Disconnected", activeDevice?.name || null, activeDevice?.id || null);
  }

  async function writeBytes(char, bytes) {
    // Check the actual advertised property flag, not just whether the
    // method exists — writeValueWithoutResponse() is present on every
    // BluetoothRemoteGATTCharacteristic regardless of what the device
    // declared, and calling it against a characteristic that only declares
    // plain WRITE (as ours do) can fail on stricter stacks.
    if (char.properties && char.properties.writeWithoutResponse) {
      await char.writeValueWithoutResponse(bytes);
    } else {
      await char.writeValue(bytes);
    }
  }

  async function writeToChar(char, text) {
    await writeBytes(char, new TextEncoder().encode(text));
  }

  function ensureOtaFileInput() {
    let input = document.getElementById("ft-ota-file-input");
    if (!input) {
      input = document.createElement("input");
      input.type = "file";
      input.accept = ".bin";
      input.id = "ft-ota-file-input";
      input.style.display = "none";
      document.body.appendChild(input);
    }
    return input;
  }

  async function runOtaUpload(file) {
    otaCancelled = false;

    if (file.size > OTA_MAX_BYTES) {
      push("onOtaStatus", "error", "Arquivo maior que o limite de 4MB");
      return;
    }

    const bytes = new Uint8Array(await file.arrayBuffer());
    if (bytes.length === 0 || bytes[0] !== 0xe9) {
      push("onOtaStatus", "error", "Arquivo inválido (assinatura ESP ausente)");
      return;
    }

    if (!cmdChar || !otaChar) {
      push("onOtaStatus", "error", "Sensor não conectado");
      return;
    }

    try {
      await writeToChar(cmdChar, `OTA:BEGIN:${bytes.length}`);

      let sent = 0;
      while (sent < bytes.length) {
        if (otaCancelled) {
          push("onOtaStatus", "idle", "Cancelado");
          return;
        }
        const chunk = bytes.subarray(sent, Math.min(sent + OTA_CHUNK_BYTES, bytes.length));
        await writeBytes(otaChar, chunk);
        sent += chunk.length;
        push("onOtaProgress", sent, bytes.length);
      }
    } catch (e) {
      push("onOtaStatus", "error", e.message || String(e));
    }
  }

  let connecting = false;

  // The page (ported from the Android app, where scanning is silent)
  // auto-calls startScan() ~1s after any disconnect or failed scan. Here each
  // call opens the browser's device picker, so unguarded it re-opened the
  // picker in a loop after the user dismissed it. Automatic calls are allowed
  // until the user dismisses the picker; after that only a real tap reopens it.
  let pickerOpen = false;
  let pickerDismissed = false;
  let lastGestureAt = 0;
  const GESTURE_WINDOW_MS = 2000;
  ["pointerdown", "touchend", "click"].forEach((type) =>
    document.addEventListener(type, () => { lastGestureAt = Date.now(); }, true)
  );

  // Rejects if a GATT step doesn't settle in time. Bluefy has been seen to
  // leave a call pending forever with the link up, which stalled the connect
  // flow silently: no notifications, no error, nothing in the debug panel.
  function withTimeout(promise, ms, label) {
    let timer;
    return Promise.race([
      promise,
      new Promise((_, reject) => {
        timer = setTimeout(() => reject(new Error(`${label} timed out after ${ms} ms`)), ms);
      }),
    ]).finally(() => clearTimeout(timer));
  }

  // True only once the whole connect flow (including startNotifications)
  // finished, so a second connect() mid-flow can't short-circuit to
  // "Connected" before notifications are enabled.
  let ready = false;

  async function connectDevice(device, address) {
    if (device.gatt.connected && ready) {
      // Already connected to this exact device — re-announce state instead
      // of redoing GATT discovery (guards against a double connect() call
      // when startScan() auto-connects and the in-app device list is also
      // tapped for the same pick).
      push("onConnectionStateChanged", "Connected", device.name || "FilterTrack", address);
      return;
    }
    if (connecting) return;
    connecting = true;
    connectDiag.length = 0;
    // Tell the page a connection is in progress: its auto-scan effect only
    // stays quiet while status is "Connecting". Without this it opened a
    // second requestDevice() picker in the middle of the GATT connect, which
    // made iOS drop the link (HCI 0x13) and the picker reappear in a loop.
    push("onConnectionStateChanged", "Connecting", device.name || "FilterTrack", address);

    device.removeEventListener("gattserverdisconnected", onGattDisconnected);
    device.addEventListener("gattserverdisconnected", onGattDisconnected);

    try {
      const server = await withTimeout(device.gatt.connect(), 15000, "gatt.connect()");
      gattServer = server;
      activeDevice = device;
      diag("gatt connected, discovering service...");

      const service = await withTimeout(server.getPrimaryService(SERVICE_UUID), 8000, "getPrimaryService()");
      cmdChar = await withTimeout(service.getCharacteristic(CHAR_CMD_NOTIFY_UUID), 8000, "getCharacteristic()");
      diag(`characteristic found, properties: notify=${cmdChar.properties.notify} write=${cmdChar.properties.write} writeNoResp=${cmdChar.properties.writeWithoutResponse}`);

      // Listener registered before startNotifications(): some WebBLE
      // polyfills (Bluefy included) are order-sensitive here. Both hooks are
      // set for polyfills that only honor one; handleNotifyOnce drops the
      // second delivery of the same event where both fire.
      cmdChar.addEventListener("characteristicvaluechanged", handleNotifyOnce);
      cmdChar.oncharacteristicvaluechanged = handleNotifyOnce;

      // startNotifications() writes the CCCD (0x2902) itself — no separate
      // getDescriptor()/CCCD write: Web Bluetooth blocklists CCCD writes,
      // CoreBluetooth forbids them, and in Bluefy getDescriptor() on the
      // CCCD never settled, stalling this whole flow.
      diag("calling startNotifications()...");
      try {
        await withTimeout(cmdChar.startNotifications(), 8000, "startNotifications()");
        diag("startNotifications() resolved OK");
      } catch (notifyErr) {
        diag(`startNotifications() FAILED: ${notifyErr.message || notifyErr}`);
      }

      try {
        otaChar = await withTimeout(service.getCharacteristic(CHAR_OTA_UUID), 3000, "getCharacteristic(OTA)");
      } catch {
        otaChar = null;
      }

      ready = true;
      push("onConnectionStateChanged", "Connected", device.name || "FilterTrack", address);
      connectDiag.forEach((msg) => push("onDataReceived", `BRIDGE:${msg}`));
      diag("connected — waiting for first notification...");
    } catch (e) {
      diag(`connect flow FAILED: ${e.message || e}`);
      push("onError", e.message || String(e));
      push("onConnectionStateChanged", "Failed", device.name || null, address);
    } finally {
      connecting = false;
    }
  }

  // Chrome never opens the device picker without a tap. Where the browser
  // exposes devices the user already allowed (getDevices(); behind
  // chrome://flags/#enable-web-bluetooth-new-permissions-backend as of Chrome
  // 151), reconnect to a known FilterTrack sensor with no tap and no picker:
  // wait for its advertising when watchAdvertisements() exists, otherwise try
  // gatt.connect() directly (the page retries after a failure).
  const BG_RETRY_MS = 5000;
  let bgSearching = false;
  let bgAbort = null;
  let lastBgAttemptAt = 0;
  let bgRetryTimer = null;

  async function reconnectPermittedDevice() {
    let devices = [];
    try {
      devices = await navigator.bluetooth.getDevices();
    } catch (e) {
      remoteLog(`getDevices failed: ${e.message || e}`);
      return false;
    }
    const device = devices.find((d) => (d.name || "").startsWith("FilterTrack"));
    remoteLog(`getDevices: ${devices.length} allowed, FilterTrack: ${device ? device.name : "none"}`);
    if (!device) return false;

    knownDevices.set(device.id, device);
    push("onScanStateChanged", true);
    push("onDeviceFound", device.name || "FilterTrack", device.id, 0);
    if (typeof device.watchAdvertisements === "function") {
      bgAbort = new AbortController();
      try {
        await new Promise((resolve, reject) => {
          device.addEventListener("advertisementreceived", resolve, { once: true });
          device.watchAdvertisements({ signal: bgAbort.signal }).catch(reject);
          bgAbort.signal.addEventListener("abort", () => reject(new Error("aborted")));
        });
      } catch (e) {
        remoteLog(`watchAdvertisements ended: ${e.message || e}`);
      }
      const aborted = bgAbort.signal.aborted;
      bgAbort.abort();
      bgAbort = null;
      if (aborted) return true; // a tap took over with the picker
    }
    push("onScanStateChanged", false);
    connectDevice(device, device.id);
    return true;
  }

  function openPicker() {
    pickerOpen = true;
    push("onScanStateChanged", true);
    // Filter by name prefix rather than service UUID: the firmware
    // advertises SERVICE_UUID as a 16-bit UUID, and some WebBLE stacks
    // (Bluefy's iOS CoreBluetooth backing included) don't reliably match
    // that against a filter expressed as the expanded 128-bit UUID, which
    // left the OS device picker permanently empty. Name-prefix matching
    // is a plain string comparison and works reliably everywhere, and it
    // also means only FilterTrack devices show in the picker at all.
    navigator.bluetooth
      .requestDevice({
        filters: [{ namePrefix: "FilterTrack" }],
        optionalServices: [SERVICE_UUID],
      })
      .then((device) => {
        pickerDismissed = false;
        knownDevices.set(device.id, device);
        push("onDeviceFound", device.name || "FilterTrack", device.id, 0);
        // Auto-connect: the OS picker tap already was the user's explicit
        // selection gesture, so there's no reason to make them tap again
        // in our own device list.
        connectDevice(device, device.id);
      })
      .catch((e) => {
        pickerDismissed = true;
        // Closing the picker (NotFoundError) is a normal choice, not an error.
        // Chrome also refuses the automatic scan on page load without a tap
        // (SecurityError: "Must be handling a user gesture"); the page's
        // "Procurar dispositivos" button is the intended path, so stay quiet.
        if (e && e.name !== "NotFoundError" && e.name !== "SecurityError") {
          push("onError", e.message || String(e));
        }
      })
      .finally(() => {
        pickerOpen = false;
        push("onScanStateChanged", false);
      });
  }

  window.Android = {
    startScan() {
      if (bleUnsupported()) {
        push("onError", "Este navegador não suporta Web Bluetooth. No iPhone, use o app Bluefy.");
        push("onScanStateChanged", false);
        return;
      }
      if (pickerOpen || connecting || (activeDevice && activeDevice.gatt.connected)) return;
      const byTap = Date.now() - lastGestureAt <= GESTURE_WINDOW_MS;

      // A tap always gets the picker (e.g. to choose another sensor),
      // cancelling any no-tap reconnect that is still waiting.
      if (byTap) {
        if (bgAbort) bgAbort.abort();
        bgSearching = false;
        openPicker();
        return;
      }
      if (bgSearching) return;

      // No tap: reconnect to an already-allowed sensor if the browser can,
      // else fall back to the picker (Bluefy opens it without a tap;
      // Chrome refuses and the page shows its "Procurar dispositivos" button).
      if (navigator.bluetooth.getDevices) {
        const wait = BG_RETRY_MS - (Date.now() - lastBgAttemptAt);
        if (wait > 0) {
          // The page only re-asks when its state changes, so keep the retry
          // loop going here while the sensor is out of reach.
          clearTimeout(bgRetryTimer);
          bgRetryTimer = setTimeout(() => window.Android.startScan(), wait);
          return;
        }
        lastBgAttemptAt = Date.now();
        bgSearching = true;
        reconnectPermittedDevice()
          .then((found) => {
            bgSearching = false;
            if (!found && !pickerDismissed) openPicker();
          })
          .catch(() => { bgSearching = false; });
        return;
      }
      if (pickerDismissed) return;
      openPicker();
    },

    stopScan() {
      push("onScanStateChanged", false);
    },

    connect(address) {
      const device = knownDevices.get(address);
      if (!device) {
        push("onError", "Dispositivo não encontrado — escaneie novamente");
        return;
      }
      connectDevice(device, address);
    },

    disconnect() {
      if (activeDevice && activeDevice.gatt && activeDevice.gatt.connected) {
        activeDevice.gatt.disconnect();
      }
    },

    sendCommand(cmd) {
      if (!cmdChar) {
        push("onError", "Sensor não conectado");
        return;
      }
      writeToChar(cmdChar, cmd).catch((e) => push("onError", e.message || String(e)));
    },

    openBiDashboard() {
      window.open(biLink(), "_blank");
    },

    openCsvGuide() {
      window.location.href = "csv-analysis.html?guide=1";
    },

    setInitialGuideActive(active) {
      try {
        localStorage.setItem("ft_guideActive", String(!!active));
      } catch {}
    },

    setGuideDataButtonsVisible(_visible) {
      // No native chrome to toggle in the browser shell; no-op.
    },

    completeInitialGuide(showNextTime) {
      try {
        localStorage.setItem("ft_showGuideOnStartup", String(!!showNextTime));
      } catch {}
    },

    setGuideStartupPreference(showOnStartup) {
      try {
        localStorage.setItem("ft_showGuideOnStartup", String(!!showOnStartup));
      } catch {}
    },

    updateFirmware() {
      if (bleUnsupported()) {
        push("onOtaStatus", "error", "Web Bluetooth não suportado neste navegador");
        return;
      }
      const input = ensureOtaFileInput();
      input.value = "";
      input.onchange = () => {
        const file = input.files && input.files[0];
        if (!file) {
          push("onOtaStatus", "idle", "Seleção cancelada");
          return;
        }
        runOtaUpload(file);
      };
      input.click();
    },

    cancelFirmwareUpdate() {
      otaCancelled = true;
    },

    isBluetoothEnabled() {
      return !bleUnsupported();
    },
  };

  // FilterTrackGuide bridge, used only by csv-analysis.html's guide flow.
  window.FilterTrackGuide = {
    openBiGuide() {
      window.open(biLink("?guide=1"), "_blank");
    },
  };

  // Mirrors the Android side's own startup behavior: a ?guide=1 URL param
  // triggers the page's own guide-start hook once it defines one.
  function maybeStartGuideFromUrl() {
    if (new URLSearchParams(location.search).get("guide") === "1") {
      window.__filterTrackGuideRequested = true;
      if (typeof window.FilterTrackStartGuide === "function") {
        window.FilterTrackStartGuide();
      }
    }
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", maybeStartGuideFromUrl);
  } else {
    maybeStartGuideFromUrl();
  }

  // Minimal cross-page nav, standing in for the native app's Activity
  // switcher (bottom nav / menu) that lived outside the WebView in Android.
  function injectNav() {
    const bar = document.createElement("div");
    bar.setAttribute("data-ft-shell-nav", "");
    bar.style.cssText =
      "position:fixed;left:8px;bottom:calc(8px + env(safe-area-inset-bottom, 0px));z-index:99999;display:flex;gap:6px;" +
      "font:12px system-ui,sans-serif;";
    const isCsv = location.pathname.endsWith("csv-analysis.html");
    bar.innerHTML = `
      <a href="index.html" style="padding:6px 10px;background:#0068B4;color:#fff;text-decoration:none;border-radius:2px;opacity:${isCsv ? 1 : 0.35}">Monitor</a>
      <a href="csv-analysis.html" data-guide="native-csv" style="padding:6px 10px;background:#0068B4;color:#fff;text-decoration:none;border-radius:2px;opacity:${isCsv ? 0.35 : 1}">CSV</a>
      <a href="${biLink()}" target="_blank" rel="noopener" style="padding:6px 10px;background:#009ca6;color:#fff;text-decoration:none;border-radius:2px;">BI</a>
      <button data-ft-help aria-label="Tutorial" title="Tutorial" style="width:28px;padding:6px 0;background:#fff;color:#0068B4;border:1px solid #0068B4;border-radius:14px;font:700 12px system-ui,sans-serif;">?</button>
    `;
    // Android only: optional native app. The button opens index.html's
    // install dialog, which explains the app already works in Chrome before
    // offering the download (window.FilterTrackOpenInstall).
    const apkAvailable = /Android/i.test(navigator.userAgent) &&
      !(window.matchMedia && window.matchMedia("(display-mode: standalone)").matches);
    if (apkAvailable) {
      const install = document.createElement("button");
      install.setAttribute("data-ft-install", "");
      install.setAttribute("aria-label", "App Android (opcional)");
      install.textContent = "⬇ App";
      install.style.cssText =
        "padding:6px 10px;background:#fff;color:#0068B4;border:1px solid #0068B4;border-radius:14px;font:600 12px system-ui,sans-serif;";
      install.onclick = () => {
        if (typeof window.FilterTrackOpenInstall === "function") window.FilterTrackOpenInstall();
        else location.href = "index.html?install=1";
      };
      bar.appendChild(install);
    }
    // The tutorial lives in index.html (window.FilterTrackOpenTutorial);
    // from other pages, go there and ask for it.
    bar.querySelector("[data-ft-help]").onclick = () => {
      if (typeof window.FilterTrackOpenTutorial === "function") window.FilterTrackOpenTutorial();
      else location.href = "index.html?tutorial=1";
    };
    document.body.appendChild(bar);
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", injectNav);
  } else {
    injectNav();
  }

  // Android: offer the native app (APK) instead of Chrome's own "Install app"
  // prompt, which would only add this web page to the home screen. The offer
  // is a card on the app's first screen (index.html's ApkInstallCard), shown
  // until the person closes it. A page can't install an APK itself — it
  // downloads it, and opening the download hands it to Android's package
  // installer (which asks to confirm, and the first time to allow installs
  // from Chrome).
  const APK_URL = "FilterTrack.apk";
  const APK_DISMISS_KEY = "ft_apkBannerDismissed";
  const isAndroid = /Android/i.test(navigator.userAgent);
  const isInstalledShell = window.matchMedia && window.matchMedia("(display-mode: standalone)").matches;

  window.addEventListener("beforeinstallprompt", (e) => {
    if (isAndroid) e.preventDefault();
  });

  window.FilterTrackApk = {
    available: isAndroid && !isInstalledShell,
    dismissed() {
      try { return localStorage.getItem(APK_DISMISS_KEY) === "1"; } catch { return false; }
    },
    dismiss() {
      try { localStorage.setItem(APK_DISMISS_KEY, "1"); } catch {}
    },
    download() {
      remoteLog("apk download started");
      const a = document.createElement("a");
      a.href = APK_URL;
      a.download = "FilterTrack.apk";
      document.body.appendChild(a);
      a.click();
      a.remove();
    },
  };
})();
