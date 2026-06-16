#!/usr/bin/env python3
"""
FilterTrack Serial Monitor
PC companion for the FilterTrack BLE sensor via USB serial.

Implements the same processing pipeline as the Android app (index.html):
  parse payload (DIST + ACC_RAW) -> IMU 20-sample rolling median -> angle correction
  -> robust 2s MAD filter -> 10s linear-regression velocity -> flow L/min

Dependencies:  pip install pyserial
"""

import json
import math
import re
import threading
import time
from collections import deque
import tkinter as tk
from tkinter import ttk, messagebox

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    import sys, subprocess
    subprocess.check_call([sys.executable, "-m", "pip", "install", "pyserial"])
    import serial
    import serial.tools.list_ports

# ── Constants (match index.html) ─────────────────────────────────────────────
MIN_VALID_DISTANCE_CM               = 25.0
MIN_MOTION_CM_PER_10S               = 0.1
MAX_REASONABLE_VELOCITY_M_PER_MIN   = 10.0
FLOW_EPSILON                        = 0.05
IMU_BUF_SIZE                        = 20
WINDOW_MS                           = 10_000
ROBUST_WINDOW_MS                    = 2_000
MAX_FILTERED_SAMPLES                = 600
MAX_HISTORY_POINTS                  = 3_600

# ── Maths helpers ─────────────────────────────────────────────────────────────
def _parse_num(value):
    try:
        f = float(str(value).replace(",", "."))
        return f if math.isfinite(f) else float("nan")
    except (ValueError, TypeError):
        return float("nan")

def _median(arr):
    if not arr:
        return float("nan")
    s = sorted(arr)
    m = (len(s) - 1) / 2
    return (s[math.floor(m)] + s[math.ceil(m)]) / 2

def _median_vec3(samples):
    if not samples:
        return None
    return {
        "x": _median([v["x"] for v in samples]),
        "y": _median([v["y"] for v in samples]),
        "z": _median([v["z"] for v in samples]),
    }

def _mag(v):
    return math.sqrt(v["x"] ** 2 + v["y"] ** 2 + v["z"] ** 2)

def _normalize(v):
    if v is None:
        return None
    n = _mag(v)
    return {"x": v["x"] / n, "y": v["y"] / n, "z": v["z"] / n} if n > 1e-6 else None

# ── IMU rolling buffer (component-wise median, ~2 s at 100 ms cadence) ────────
class _ImuBuffer:
    def __init__(self):
        self.accel: deque = deque(maxlen=IMU_BUF_SIZE)

    def reset(self):
        self.accel.clear()

    def push(self, accel_raw):
        if accel_raw:
            self.accel.append(accel_raw)

    def filtered_accel(self):
        buf = list(self.accel)
        if not buf:
            return None
        return buf[-1] if len(buf) < 2 else _median_vec3(buf)

# ── Angle & vertical-distance correction ──────────────────────────────────────
def _angle_from_ground_deg(accel_raw):
    g = _normalize(accel_raw)
    if not g:
        return float("nan")
    dot = max(-1.0, min(1.0, g["z"]))
    return math.degrees(math.acos(abs(dot)))

def _correct_distance(distance_cm, angle_deg):
    if not (math.isfinite(distance_cm) and math.isfinite(angle_deg)):
        return distance_cm
    return distance_cm * math.sin(math.radians(max(0.0, min(90.0, angle_deg))))

# ── Payload parser ─────────────────────────────────────────────────────────────
def _pick_vec_from_text(text, keys):
    for key in keys:
        pat = (rf'{re.escape(key)}\s*[:=]\s*'
               r'(-?\d+(?:[.,]\d+)?)\s*,\s*(-?\d+(?:[.,]\d+)?)\s*,\s*(-?\d+(?:[.,]\d+)?)')
        m = re.search(pat, text, re.IGNORECASE)
        if m:
            x, y, z = _parse_num(m.group(1)), _parse_num(m.group(2)), _parse_num(m.group(3))
            if all(math.isfinite(c) for c in (x, y, z)):
                return {"x": x, "y": y, "z": z}
    return None

def _pick_dist_from_text(text):
    for pat in (
        r'DIST\s*=\s*(-?\d+(?:[.,]\d+)?)',
        r'^D\s*=\s*(-?\d+(?:[.,]\d+)?)',
    ):
        m = re.search(pat, text, re.IGNORECASE)
        if m:
            d = _parse_num(m.group(1))
            if math.isfinite(d):
                return d
    d = _parse_num(text.strip())
    if math.isfinite(d):
        return d
    m = re.search(r'-?\d+(?:[.,]\d+)?', text)
    if m:
        d = _parse_num(m.group(0))
        if math.isfinite(d):
            return d
    return float("nan")

def parse_payload(raw: str, imu: _ImuBuffer) -> dict:
    text = str(raw or "").strip()
    nan = float("nan")
    if not text:
        return {"distance": nan, "raw_distance": nan, "angle_deg": nan, "accel_error": False, "error": "payload vazio"}
    if re.match(r"^(err|erro)", text, re.IGNORECASE):
        return {"distance": nan, "raw_distance": nan, "angle_deg": nan, "accel_error": False, "error": text}

    dist_erro  = bool(re.search(r"(?:^|;)\s*(?:dist|d)\s*=\s*erro\b", text, re.IGNORECASE))
    accel_erro = bool(re.search(r"\braw\s*=\s*erro\b", text, re.IGNORECASE))

    accel_raw = None if accel_erro else _pick_vec_from_text(text, ["ACC_RAW", "ACCEL_RAW", "ACCEL", "ACC", "A"])

    if text.startswith("{") or text.startswith("["):
        try:
            o = json.loads(text)
            for k in ["distance", "dist", "distance_cm", "dist_cm", "distanceCm",
                      "distancia", "distancia_cm"]:
                if k in o:
                    d = _parse_num(o[k])
                    if math.isfinite(d):
                        imu.push(accel_raw)
                        fa = imu.filtered_accel()
                        ang = _angle_from_ground_deg(fa)
                        return {
                            "distance":    _correct_distance(d, ang),
                            "raw_distance": d,
                            "angle_deg":   ang,
                            "accel_error": accel_erro,
                            "error":       None,
                        }
        except (json.JSONDecodeError, TypeError):
            pass

    if dist_erro:
        imu.push(accel_raw)
        return {"distance": nan, "raw_distance": nan, "angle_deg": nan,
                "accel_error": accel_erro, "error": "DIST=ERRO"}

    raw_dist = _pick_dist_from_text(text)
    if not math.isfinite(raw_dist):
        return {"distance": nan, "raw_distance": nan, "angle_deg": nan,
                "accel_error": accel_erro, "error": "payload sem distancia valida"}

    imu.push(accel_raw)
    fa  = imu.filtered_accel()
    ang = _angle_from_ground_deg(fa)
    return {
        "distance":    _correct_distance(raw_dist, ang),
        "raw_distance": raw_dist,
        "angle_deg":   ang,
        "accel_error": accel_erro,
        "error":       None,
    }

# ── Robust 2-second MAD filter ────────────────────────────────────────────────
def robust_mean(samples: list, window_ms: float, now_ms: float) -> float:
    values = [s["distance"] for s in samples
              if s["ts"] >= now_ms - window_ms and math.isfinite(s["distance"])]
    if not values:
        return float("nan")
    if len(values) < 3:
        return values[-1]
    med    = _median(values)
    mad    = _median([abs(v - med) for v in values])
    thresh = max(mad * 3, 0.5)
    inliers = [v for v in values if abs(v - med) <= thresh]
    return sum(inliers) / len(inliers) if inliers else med

# ── Linear regression slope (m/s) ─────────────────────────────────────────────
def _slope_m_per_s(samples: list) -> float:
    if len(samples) < 2:
        return float("nan")
    t0 = samples[0]["ts"]
    n = sx = sy = sxx = sxy = 0
    for s in samples:
        if not (math.isfinite(s["distance"]) and math.isfinite(s["ts"])):
            continue
        x = (s["ts"] - t0) / 1000.0
        y = s["distance"] / 100.0
        n += 1; sx += x; sy += y; sxx += x * x; sxy += x * y
    if n < 2:
        return float("nan")
    d = n * sxx - sx * sx
    return (n * sxy - sx * sy) / d if abs(d) >= 1e-9 else float("nan")

# ── 10-second window velocity ─────────────────────────────────────────────────
def compute_motion(samples: list, now_ms: float) -> dict:
    def _empty(r):
        return {"velocity_mpm": float("nan"), "motion_cm": float("nan"), "reason": r}

    if len(samples) < 2:
        return _empty("Aguardando pelo menos 2 amostras.")
    win = [s for s in samples if s["ts"] >= now_ms - WINDOW_MS]
    if len(win) < 2:
        return _empty("Aguardando amostras recentes.")
    first, last = win[0], win[-1]
    if (last["ts"] - first["ts"]) < WINDOW_MS * 0.9:
        return _empty("Janela de 10s ainda incompleta.")
    motion_cm = last["distance"] - first["distance"]
    if not math.isfinite(motion_cm):
        return _empty("Movimento invalido.")
    if abs(motion_cm) < MIN_MOTION_CM_PER_10S:
        return _empty(f"Movimento abaixo de {MIN_MOTION_CM_PER_10S} cm em 10s.")
    slope = _slope_m_per_s(win)
    if not math.isfinite(slope):
        return _empty("Inclinacao sem dados suficientes.")
    vmpm = -slope * 60
    if not math.isfinite(vmpm):
        return _empty("Velocidade invalida.")
    if abs(vmpm) > MAX_REASONABLE_VELOCITY_M_PER_MIN:
        return _empty(f"Velocidade acima de {MAX_REASONABLE_VELOCITY_M_PER_MIN} m/min.")
    return {"velocity_mpm": vmpm, "motion_cm": motion_cm, "reason": "Janela de 10s valida."}

def flow_lpm(area_m2: float, velocity_mpm: float) -> float:
    if not (math.isfinite(area_m2) and area_m2 > 0 and math.isfinite(velocity_mpm)):
        return float("nan")
    return area_m2 * velocity_mpm * 1000

def classify(f: float):
    if not math.isfinite(f):
        return "Sem dados", "neutral"
    if f < -FLOW_EPSILON:
        return "Descendo ↓", "danger"
    if f > FLOW_EPSILON:
        return "Subindo ↑", "success"
    return "Estavel", "neutral"

# ── Velocity mini-chart (canvas) ──────────────────────────────────────────────
class VelocityChart(tk.Canvas):
    W, H = 660, 100

    def __init__(self, parent, **kw):
        super().__init__(parent, width=self.W, height=self.H,
                         bg="#1e1e1e", highlightthickness=0, **kw)
        self._history: list = []   # list of velocity_mpm floats

    def push(self, vmpm: float):
        self._history.append(vmpm)
        if len(self._history) > MAX_HISTORY_POINTS:
            self._history = self._history[-MAX_HISTORY_POINTS:]
        self._draw()

    def reset(self):
        self._history.clear()
        self.delete("all")

    def _draw(self):
        self.delete("all")
        pts = [v for v in self._history if math.isfinite(v)]
        if len(pts) < 2:
            return
        mn, mx = min(pts), max(pts)
        span = mx - mn or 1.0
        w, h = self.W, self.H
        pad = 8

        def _y(v):
            return h - pad - (v - mn) / span * (h - 2 * pad)

        # zero line
        if mn <= 0 <= mx:
            yz = _y(0)
            self.create_line(0, yz, w, yz, fill="#444", dash=(4, 4))

        # data line
        step = max(1, len(pts) // w)
        coords = []
        for i, v in enumerate(pts[::step]):
            x = i / (len(pts[::step]) - 1) * w
            coords += [x, _y(v)]
        if len(coords) >= 4:
            color = "#42be65" if pts[-1] >= 0 else "#ff8389"
            self.create_line(*coords, fill=color, width=1.5)

# ── Main Application ──────────────────────────────────────────────────────────
BAUD_RATES = [9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600]
TONE_COLOR = {"success": "#24a148", "danger": "#da1e28", "neutral": "#525252"}

class App:
    def __init__(self, root: tk.Tk):
        self.root = root
        root.title("FilterTrack Serial Monitor")
        root.resizable(True, True)
        root.configure(bg="#f4f4f4")

        self._serial:  serial.Serial | None = None
        self._thread:  threading.Thread | None = None
        self._running = False
        self._imu     = _ImuBuffer()
        self._raw:    deque = deque(maxlen=MAX_FILTERED_SAMPLES)
        self._filt:   deque = deque(maxlen=MAX_FILTERED_SAMPLES)
        self._first_done   = False
        self._win_start_ms: float | None = None
        self._last_sign    = 0

        self._build_ui()
        self._refresh_ports()

    # ── UI ────────────────────────────────────────────────────────────────────
    def _build_ui(self):
        bg = "#f4f4f4"
        pad = dict(padx=12, pady=6)

        # Connection bar
        bar = tk.Frame(self.root, bg=bg)
        bar.pack(fill="x", **pad)

        tk.Label(bar, text="Porta:", bg=bg).pack(side="left")
        self._port_var = tk.StringVar()
        self._port_cb  = ttk.Combobox(bar, textvariable=self._port_var,
                                      width=14, state="readonly")
        self._port_cb.pack(side="left", padx=4)

        tk.Label(bar, text="Baud:", bg=bg).pack(side="left", padx=(8, 0))
        self._baud_var = tk.StringVar(value="115200")
        ttk.Combobox(bar, textvariable=self._baud_var,
                     values=[str(b) for b in BAUD_RATES],
                     width=10, state="readonly").pack(side="left", padx=4)

        tk.Label(bar, text="Área filtro (m²):", bg=bg).pack(side="left", padx=(12, 0))
        self._area_var = tk.StringVar(value="1.0")
        tk.Entry(bar, textvariable=self._area_var, width=8).pack(side="left", padx=4)

        self._conn_btn = tk.Button(bar, text="Conectar",
                                   command=self._toggle,
                                   bg="#0f62fe", fg="white",
                                   relief="flat", padx=12, pady=3,
                                   cursor="hand2")
        self._conn_btn.pack(side="left", padx=(10, 4))

        tk.Button(bar, text="↻", command=self._refresh_ports,
                  bg="#e0e0e0", relief="flat", padx=6, pady=3,
                  cursor="hand2").pack(side="left")

        # Metrics grid
        grid = tk.Frame(self.root, bg=bg)
        grid.pack(fill="x", padx=12, pady=4)

        rows = [
            ("dist_raw",    "Distância ESP32 (cm)"),
            ("accel_state", "Acelerômetro"),
            ("angle_deg",   "Ângulo sensor/solo (°)"),
            ("dist_corr",   "Distância vertical (cm)"),
            ("dist_filt",   "Pós-filtro MAD (cm)"),
            ("velocity",    "Velocidade (m/min)"),
            ("flow_lpm",    "Vazão (L/min)"),
            ("flow_m3h",    "Vazão (m³/h)"),
            ("direction",   "Direção"),
            ("status",      "Status"),
        ]
        self._vars: dict[str, tuple[tk.StringVar, tk.Label]] = {}
        for i, (key, label) in enumerate(rows):
            tk.Label(grid, text=label, bg=bg, anchor="e", width=28,
                     font=("TkDefaultFont", 10)).grid(row=i, column=0, sticky="e", pady=2)
            var = tk.StringVar(value="--")
            lbl = tk.Label(grid, textvariable=var, bg=bg, anchor="w", width=24,
                           font=("TkFixedFont", 11, "bold"), fg="#525252")
            lbl.grid(row=i, column=1, sticky="w", padx=10, pady=2)
            self._vars[key] = (var, lbl)

        # Chart
        chart_frame = tk.Frame(self.root, bg=bg)
        chart_frame.pack(fill="x", padx=12, pady=(0, 4))
        tk.Label(chart_frame, text="Velocidade (m/min)", bg=bg,
                 font=("TkDefaultFont", 9), fg="#525252").pack(anchor="w")
        self._chart = VelocityChart(chart_frame)
        self._chart.pack()

        # Log
        log_frame = tk.Frame(self.root, bg=bg)
        log_frame.pack(fill="both", expand=True, padx=12, pady=(0, 8))
        tk.Label(log_frame, text="Log serial:", bg=bg,
                 font=("TkDefaultFont", 9), fg="#525252").pack(anchor="w")
        self._log = tk.Text(log_frame, height=7, font=("TkFixedFont", 9),
                            state="disabled", bg="#1e1e1e", fg="#d4d4d4",
                            relief="flat", wrap="none")
        sb = ttk.Scrollbar(log_frame, command=self._log.yview)
        self._log["yscrollcommand"] = sb.set
        sb.pack(side="right", fill="y")
        self._log.pack(fill="both", expand=True)

    def _set(self, key: str, value: str, color: str | None = None):
        var, lbl = self._vars[key]
        var.set(value)
        if color:
            lbl.configure(fg=color)

    def _log_line(self, text: str):
        self._log.configure(state="normal")
        self._log.insert("end", text + "\n")
        self._log.see("end")
        n = int(self._log.index("end-1c").split(".")[0])
        if n > 300:
            self._log.delete("1.0", "100.0")
        self._log.configure(state="disabled")

    # ── Port management ───────────────────────────────────────────────────────
    def _refresh_ports(self):
        ports = [p.device for p in serial.tools.list_ports.comports()]
        self._port_cb["values"] = ports
        if ports:
            self._port_var.set(ports[0])

    def _toggle(self):
        if self._running:
            self._disconnect()
        else:
            self._connect()

    def _connect(self):
        port = self._port_var.get()
        if not port:
            messagebox.showerror("Erro", "Selecione uma porta serial.")
            return
        baud = int(self._baud_var.get())
        try:
            self._serial = serial.Serial(port, baud, timeout=1)
        except serial.SerialException as exc:
            messagebox.showerror("Erro ao conectar", str(exc))
            return
        self._running = True
        self._reset_state()
        self._conn_btn.configure(text="Desconectar", bg="#da1e28")
        self._log_line(f"Conectado em {port} @ {baud} baud")
        self._thread = threading.Thread(target=self._read_loop, daemon=True)
        self._thread.start()

    def _disconnect(self):
        self._running = False
        if self._serial:
            try:
                self._serial.close()
            except Exception:
                pass
            self._serial = None
        self._conn_btn.configure(text="Conectar", bg="#0f62fe")
        self._log_line("Desconectado.")

    def _reset_state(self):
        self._imu.reset()
        self._raw.clear()
        self._filt.clear()
        self._first_done    = False
        self._win_start_ms  = None
        self._last_sign     = 0
        self._chart.reset()
        for key in self._vars:
            self._set(key, "--", "#525252")

    # ── Serial read loop (background thread) ──────────────────────────────────
    def _read_loop(self):
        buf = ""
        while self._running:
            try:
                chunk = self._serial.read(256).decode("utf-8", errors="replace")
                buf += chunk
                while "\n" in buf:
                    line, buf = buf.split("\n", 1)
                    line = line.strip()
                    if line:
                        self.root.after(0, self._on_line, line)
            except serial.SerialException:
                self.root.after(0, self._on_error)
                break
            except Exception:
                pass

    def _on_error(self):
        self._log_line("Erro de comunicação serial.")
        self._disconnect()

    # ── Processing pipeline ───────────────────────────────────────────────────
    def _on_line(self, raw: str):
        self._log_line(raw)
        now_ms = time.time() * 1000

        parsed = parse_payload(raw, self._imu)

        accel_err = parsed.get("accel_error", False)
        self._set("accel_state",
                  "ERRO" if accel_err else "OK",
                  TONE_COLOR["danger"] if accel_err else "#24a148")

        if parsed.get("error") and parsed["error"] != "DIST=ERRO":
            self._set("status", f"Erro: {parsed['error']}", TONE_COLOR["danger"])
            return

        raw_dist = parsed["raw_distance"]
        angle    = parsed["angle_deg"]
        corrected = parsed["distance"]

        # Discard below threshold
        if math.isfinite(raw_dist) and raw_dist < MIN_VALID_DISTANCE_CM:
            self._set("status", f"DIST raw {raw_dist:.1f} cm < {MIN_VALID_DISTANCE_CM} cm — ignorado",
                      "#8a3ffc")
            return

        if not math.isfinite(corrected):
            return

        # Ignore first reading after connect
        if not self._first_done:
            self._first_done = True
            self._set("status", "Primeira leitura descartada.", "#525252")
            return

        self._raw.append({"ts": now_ms, "distance": corrected})

        # Robust 2-second MAD filter
        filtered = robust_mean(list(self._raw), ROBUST_WINDOW_MS, now_ms)
        if not math.isfinite(filtered):
            return

        self._filt.append({"ts": now_ms, "distance": filtered})

        # Area
        try:
            area_m2 = float(self._area_var.get().replace(",", "."))
        except ValueError:
            area_m2 = float("nan")

        if self._win_start_ms is None:
            self._win_start_ms = now_ms

        # Motion & flow
        motion   = compute_motion(list(self._filt), now_ms)
        vmpm     = motion["velocity_mpm"]
        f_lpm    = flow_lpm(area_m2, vmpm)
        label, tone = classify(f_lpm)
        color    = TONE_COLOR[tone]

        # Flow direction inversion → reset history & countdown
        cur_sign = (0 if not math.isfinite(vmpm)
                    else (1 if vmpm > 0 else (-1 if vmpm < 0 else 0)))
        if self._last_sign != 0 and cur_sign != 0 and cur_sign != self._last_sign:
            self._filt.clear()
            self._filt.append({"ts": now_ms, "distance": filtered})
            self._win_start_ms = now_ms
            self._chart.reset()
        if cur_sign != 0:
            self._last_sign = cur_sign

        elapsed   = now_ms - self._win_start_ms
        collecting = elapsed < WINDOW_MS
        remaining  = max(0, math.ceil((WINDOW_MS - elapsed) / 1000))

        # Push chart point
        if math.isfinite(vmpm):
            self._chart.push(vmpm)

        # Update metrics
        self._set("dist_raw",  f"{raw_dist:.2f}" if math.isfinite(raw_dist) else "--")
        self._set("angle_deg", f"{angle:.2f}°"   if math.isfinite(angle)    else "--")
        self._set("dist_corr", f"{corrected:.2f}")
        self._set("dist_filt", f"{filtered:.2f}")

        if collecting:
            wait = f"Coletando ({remaining}s)..."
            self._set("velocity",  wait,  "#8a3ffc")
            self._set("flow_lpm",  wait,  "#8a3ffc")
            self._set("flow_m3h",  "--")
            self._set("direction", "--")
            self._set("status",    motion["reason"], "#525252")
        elif not math.isfinite(vmpm):
            self._set("velocity",  "--")
            self._set("flow_lpm",  "--")
            self._set("flow_m3h",  "--")
            self._set("direction", "--")
            self._set("status",    motion["reason"], TONE_COLOR["danger"])
        else:
            f_m3h = f_lpm * 0.06 if math.isfinite(f_lpm) else float("nan")
            self._set("velocity",  f"{vmpm:.2f}",  "#1e1e1e")
            self._set("flow_lpm",  f"{f_lpm:.1f}"  if math.isfinite(f_lpm)  else "--", color)
            self._set("flow_m3h",  f"{f_m3h:.3f}"  if math.isfinite(f_m3h)  else "--", color)
            self._set("direction", label,  color)
            self._set("status",    motion["reason"], "#525252")

# ── Entry point ───────────────────────────────────────────────────────────────
if __name__ == "__main__":
    root = tk.Tk()
    App(root)
    root.mainloop()
