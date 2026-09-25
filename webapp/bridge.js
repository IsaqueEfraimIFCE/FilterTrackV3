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
    push("onDataReceived", `BRIDGE:${msg}`);
  }

  function push(method, ...args) {
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

  function handleNotify(event) {
    const value = event.target.value;
    const text = new TextDecoder("utf-8").decode(value);
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

  function onGattDisconnected() {
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

  async function connectDevice(device, address) {
    if (device.gatt.connected && cmdChar) {
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
      const server = await device.gatt.connect();
      gattServer = server;
      activeDevice = device;
      diag("gatt connected, discovering service...");

      const service = await server.getPrimaryService(SERVICE_UUID);
      cmdChar = await service.getCharacteristic(CHAR_CMD_NOTIFY_UUID);
      diag(`characteristic found, properties: notify=${cmdChar.properties.notify} write=${cmdChar.properties.write} writeNoResp=${cmdChar.properties.writeWithoutResponse}`);

      // Listener registered before startNotifications(): some WebBLE
      // polyfills (Bluefy included) are order-sensitive here.
      cmdChar.addEventListener("characteristicvaluechanged", handleNotify);
      cmdChar.oncharacteristicvaluechanged = handleNotify;

      // startNotifications() writes the CCCD (0x2902) itself. Never write the
      // CCCD directly: Web Bluetooth blocklists it and CoreBluetooth (Bluefy)
      // forbids it. The firmware must expose a CCCD; without one iOS can't
      // subscribe and drops every notification while still showing
      // "Connected".
      try {
        await cmdChar.getDescriptor(CCCD_UUID);
        diag("CCCD present on device");
      } catch {
        diag("CCCD MISSING on device — iOS can't subscribe; update the firmware");
      }

      try {
        await cmdChar.startNotifications();
        diag("startNotifications() resolved OK");
      } catch (notifyErr) {
        diag(`startNotifications() FAILED: ${notifyErr.message || notifyErr}`);
      }

      try {
        otaChar = await service.getCharacteristic(CHAR_OTA_UUID);
      } catch {
        otaChar = null;
      }

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

  window.Android = {
    startScan() {
      if (bleUnsupported()) {
        push("onError", "Este navegador não suporta Web Bluetooth. No iPhone, use o app Bluefy.");
        push("onScanStateChanged", false);
        return;
      }
      if (pickerOpen || connecting || (activeDevice && activeDevice.gatt.connected)) return;
      if (pickerDismissed && Date.now() - lastGestureAt > GESTURE_WINDOW_MS) return;
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
          if (e && e.name !== "NotFoundError") push("onError", e.message || String(e));
        })
        .finally(() => {
          pickerOpen = false;
          push("onScanStateChanged", false);
        });
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
      window.open(BI_URL, "_blank");
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
      window.open(`${BI_URL}?guide=1`, "_blank");
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
      "position:fixed;left:8px;bottom:8px;z-index:99999;display:flex;gap:6px;" +
      "font:12px system-ui,sans-serif;";
    const isCsv = location.pathname.endsWith("csv-analysis.html");
    bar.innerHTML = `
      <a href="index.html" style="padding:6px 10px;background:#0068B4;color:#fff;text-decoration:none;border-radius:2px;opacity:${isCsv ? 1 : 0.35}">Monitor</a>
      <a href="csv-analysis.html" style="padding:6px 10px;background:#0068B4;color:#fff;text-decoration:none;border-radius:2px;opacity:${isCsv ? 0.35 : 1}">CSV</a>
      <a href="${BI_URL}" target="_blank" rel="noopener" style="padding:6px 10px;background:#009ca6;color:#fff;text-decoration:none;border-radius:2px;">BI</a>
    `;
    document.body.appendChild(bar);
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", injectNav);
  } else {
    injectNav();
  }
})();
