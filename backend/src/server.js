import { createHash, randomUUID } from "node:crypto";
import { createServer } from "node:http";
import { appendFile, mkdir, readFile, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

const HOST = process.env.HOST || "0.0.0.0";
const PORT = readPositiveIntEnv("PORT", 8080);
const DATA_DIR = path.resolve(process.env.DATA_DIR || path.join(__dirname, "..", "data"));
const DATA_FILE = path.join(DATA_DIR, "sessions.ndjson");
const MAX_PAYLOAD_BYTES = readPositiveIntEnv("MAX_PAYLOAD_MB", 4) * 1024 * 1024;
const MAX_SAMPLES_PER_SESSION = readPositiveIntEnv("MAX_SAMPLES_PER_SESSION", 120000);
const API_KEY = (process.env.FILTERTRACK_API_KEY || "").trim();
const CORS_ALLOW_ORIGIN = process.env.CORS_ALLOW_ORIGIN || "*";

const sessions = [];
const dedupMap = new Map();

function readPositiveIntEnv(name, fallback) {
  const parsed = Number.parseInt(process.env[name] || "", 10);
  return Number.isInteger(parsed) && parsed > 0 ? parsed : fallback;
}

function parseIsoOrNull(value) {
  if (typeof value !== "string" || !value.trim()) return null;
  const d = new Date(value);
  if (Number.isNaN(d.getTime())) return null;
  return d.toISOString();
}

function toSafeString(value, maxLength = 256) {
  if (value == null) return "";
  const text = String(value).trim();
  if (!text) return "";
  return text.length <= maxLength ? text : text.slice(0, maxLength);
}

function parseDateParamOrThrow(value, paramName) {
  if (value == null || value === "") return null;
  const date = new Date(value);
  if (Number.isNaN(date.getTime())) {
    throw httpError(400, `Parametro '${paramName}' invalido. Use ISO-8601.`);
  }
  return date.getTime();
}

function parseLimitOrDefault(value, fallback, min, max) {
  const parsed = Number.parseInt(value || "", 10);
  if (!Number.isInteger(parsed)) return fallback;
  return Math.max(min, Math.min(max, parsed));
}

function requestUrl(req) {
  const host = req.headers.host || `${HOST}:${PORT}`;
  return new URL(req.url || "/", `http://${host}`);
}

function getRecordTimeMs(record) {
  const startedAt = parseIsoOrNull(record?.session?.startedAt);
  if (startedAt) return Date.parse(startedAt);
  const endedAt = parseIsoOrNull(record?.session?.endedAt);
  if (endedAt) return Date.parse(endedAt);
  return Date.parse(record.receivedAt);
}

function normalizeDevice(rawDevice) {
  if (!rawDevice || typeof rawDevice !== "object" || Array.isArray(rawDevice)) {
    return { name: "", address: "" };
  }
  return {
    name: toSafeString(rawDevice.name, 160),
    address: toSafeString(rawDevice.address, 160),
  };
}

function normalizeFilter(rawFilter) {
  if (!rawFilter || typeof rawFilter !== "object" || Array.isArray(rawFilter)) {
    return null;
  }
  return {
    id: toSafeString(rawFilter.id, 160),
    name: toSafeString(rawFilter.name, 200),
    areaM2: Number.isFinite(Number(rawFilter.areaM2)) ? Number(rawFilter.areaM2) : null,
    location: toSafeString(rawFilter.location, 200),
    businessUnit: toSafeString(rawFilter.businessUnit, 200),
  };
}

function dedupKeyFor(envelope) {
  const sessionId = toSafeString(envelope?.session?.id, 200);
  if (sessionId) return `${envelope.app}:${sessionId}`;

  const fallback = JSON.stringify({
    app: envelope.app,
    startedAt: envelope.session?.startedAt || "",
    endedAt: envelope.session?.endedAt || "",
    deviceAddress: envelope.session?.device?.address || "",
    sampleCount: envelope.session?.sampleCount || 0,
  });
  return `${envelope.app}:hash:${createHash("sha256").update(fallback).digest("hex")}`;
}

function normalizeIncomingPayload(payload) {
  if (!payload || typeof payload !== "object" || Array.isArray(payload)) {
    throw httpError(400, "Payload deve ser um objeto JSON.");
  }

  const rawSession = payload.session;
  if (!rawSession || typeof rawSession !== "object" || Array.isArray(rawSession)) {
    throw httpError(400, "Campo 'session' obrigatorio no payload.");
  }

  const samples = Array.isArray(rawSession.samples) ? rawSession.samples : [];
  if (samples.length > MAX_SAMPLES_PER_SESSION) {
    throw httpError(
      413,
      `Sessao excede limite de ${MAX_SAMPLES_PER_SESSION} amostras.`
    );
  }

  const normalizedSession = {
    ...rawSession,
    id: toSafeString(rawSession.id, 200) || randomUUID(),
    startedAt: parseIsoOrNull(rawSession.startedAt),
    endedAt: parseIsoOrNull(rawSession.endedAt),
    endReason: toSafeString(rawSession.endReason, 200),
    sampleCount:
      Number.isInteger(rawSession.sampleCount) && rawSession.sampleCount >= 0
        ? rawSession.sampleCount
        : samples.length,
    device: normalizeDevice(rawSession.device),
    filter: normalizeFilter(rawSession.filter),
    samples,
  };

  return {
    schemaVersion: Number.isFinite(Number(payload.schemaVersion))
      ? Number(payload.schemaVersion)
      : 1,
    app: toSafeString(payload.app, 80) || "FilterTrack",
    uploadedAt: parseIsoOrNull(payload.uploadedAt) || new Date().toISOString(),
    session: normalizedSession,
  };
}

function normalizeStoredRecord(rawRecord) {
  if (!rawRecord || typeof rawRecord !== "object" || Array.isArray(rawRecord)) {
    return null;
  }

  const normalized = normalizeIncomingPayload({
    schemaVersion: rawRecord.schemaVersion,
    app: rawRecord.app,
    uploadedAt: rawRecord.uploadedAt,
    session: rawRecord.session || {},
  });

  return {
    ingestionId: toSafeString(rawRecord.ingestionId, 80) || randomUUID(),
    dedupKey: toSafeString(rawRecord.dedupKey, 400),
    receivedAt: parseIsoOrNull(rawRecord.receivedAt) || new Date().toISOString(),
    ...normalized,
  };
}

function toSessionSummary(record) {
  return {
    ingestionId: record.ingestionId,
    sessionId: record.session.id,
    app: record.app,
    schemaVersion: record.schemaVersion,
    receivedAt: record.receivedAt,
    uploadedAt: record.uploadedAt,
    startedAt: record.session.startedAt,
    endedAt: record.session.endedAt,
    endReason: record.session.endReason || null,
    sampleCount: record.session.sampleCount,
    device: record.session.device,
    filter: record.session.filter,
  };
}

function parseQueryFilters(searchParams) {
  const fromMs = parseDateParamOrThrow(searchParams.get("from"), "from");
  const toMs = parseDateParamOrThrow(searchParams.get("to"), "to");
  const deviceAddress = toSafeString(searchParams.get("deviceAddress"), 160).toLowerCase();
  const filterId = toSafeString(searchParams.get("filterId"), 160).toLowerCase();

  if (fromMs != null && toMs != null && fromMs > toMs) {
    throw httpError(400, "Parametro 'from' deve ser menor ou igual a 'to'.");
  }

  return {
    fromMs,
    toMs,
    deviceAddress,
    filterId,
  };
}

function applyRecordFilters(records, filters) {
  return records.filter((record) => {
    const t = getRecordTimeMs(record);
    if (filters.fromMs != null && t < filters.fromMs) return false;
    if (filters.toMs != null && t > filters.toMs) return false;

    if (filters.deviceAddress) {
      const address = toSafeString(record.session?.device?.address, 160).toLowerCase();
      if (address !== filters.deviceAddress) return false;
    }
    if (filters.filterId) {
      const id = toSafeString(record.session?.filter?.id, 160).toLowerCase();
      if (id !== filters.filterId) return false;
    }
    return true;
  });
}

function buildStats(records) {
  if (!records.length) {
    return {
      totalSessions: 0,
      totalSamples: 0,
      firstSessionAt: null,
      lastSessionAt: null,
      topDevices: [],
      topFilters: [],
    };
  }

  let totalSamples = 0;
  let firstMs = Number.POSITIVE_INFINITY;
  let lastMs = 0;
  const devices = new Map();
  const filters = new Map();

  for (const record of records) {
    totalSamples += Number.isInteger(record.session.sampleCount) ? record.session.sampleCount : 0;
    const t = getRecordTimeMs(record);
    if (t < firstMs) firstMs = t;
    if (t > lastMs) lastMs = t;

    const address = toSafeString(record.session?.device?.address, 160) || "desconhecido";
    devices.set(address, (devices.get(address) || 0) + 1);

    const filterId = toSafeString(record.session?.filter?.id, 160) || "sem-filtro";
    filters.set(filterId, (filters.get(filterId) || 0) + 1);
  }

  const topDevices = Array.from(devices.entries())
    .sort((a, b) => b[1] - a[1])
    .slice(0, 10)
    .map(([address, count]) => ({ address, count }));

  const topFilters = Array.from(filters.entries())
    .sort((a, b) => b[1] - a[1])
    .slice(0, 10)
    .map(([id, count]) => ({ id, count }));

  return {
    totalSessions: records.length,
    totalSamples,
    firstSessionAt: Number.isFinite(firstMs) ? new Date(firstMs).toISOString() : null,
    lastSessionAt: lastMs > 0 ? new Date(lastMs).toISOString() : null,
    topDevices,
    topFilters,
  };
}

function httpError(statusCode, message) {
  const error = new Error(message);
  error.statusCode = statusCode;
  return error;
}

function withCors(res) {
  res.setHeader("Access-Control-Allow-Origin", CORS_ALLOW_ORIGIN);
  res.setHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  res.setHeader("Access-Control-Allow-Headers", "Content-Type,X-API-Key");
}

function sendJson(res, statusCode, payload) {
  withCors(res);
  res.writeHead(statusCode, { "Content-Type": "application/json; charset=utf-8" });
  res.end(JSON.stringify(payload));
}

function sendError(res, statusCode, message) {
  sendJson(res, statusCode, {
    ok: false,
    error: message,
  });
}

function isAuthorized(req) {
  if (!API_KEY) return true;
  const header = req.headers["x-api-key"];
  return typeof header === "string" && header === API_KEY;
}

function readJsonBody(req) {
  return new Promise((resolve, reject) => {
    let size = 0;
    let data = "";
    let closed = false;

    req.on("data", (chunk) => {
      if (closed) return;
      size += chunk.length;
      if (size > MAX_PAYLOAD_BYTES) {
        closed = true;
        reject(httpError(413, `Payload excede ${MAX_PAYLOAD_BYTES} bytes.`));
        req.destroy();
        return;
      }
      data += chunk;
    });

    req.on("end", () => {
      if (closed) return;
      if (!data.trim()) {
        reject(httpError(400, "Payload vazio."));
        return;
      }
      try {
        resolve(JSON.parse(data));
      } catch (_) {
        reject(httpError(400, "JSON invalido."));
      }
    });

    req.on("error", (err) => {
      if (closed) return;
      closed = true;
      reject(err);
    });
  });
}

async function appendRecord(record) {
  await appendFile(DATA_FILE, `${JSON.stringify(record)}\n`, "utf8");
}

async function loadStore() {
  await mkdir(DATA_DIR, { recursive: true });

  let raw = "";
  try {
    raw = await readFile(DATA_FILE, "utf8");
  } catch (err) {
    if (err && err.code === "ENOENT") {
      await writeFile(DATA_FILE, "", "utf8");
    } else {
      throw err;
    }
  }

  if (!raw.trim()) return;

  let skipped = 0;
  const lines = raw.split(/\r?\n/);
  for (const line of lines) {
    if (!line.trim()) continue;
    try {
      const parsed = JSON.parse(line);
      const record = normalizeStoredRecord(parsed);
      if (!record) {
        skipped += 1;
        continue;
      }

      if (!record.dedupKey) {
        record.dedupKey = dedupKeyFor(record);
      }

      sessions.push(record);
      dedupMap.set(record.dedupKey, record);
    } catch (_) {
      skipped += 1;
    }
  }

  if (skipped > 0) {
    console.warn(`[store] ${skipped} linha(s) invalidas ignoradas em ${DATA_FILE}`);
  }
}

function handleGetHealth(res) {
  sendJson(res, 200, {
    ok: true,
    status: "healthy",
    time: new Date().toISOString(),
    sessionCount: sessions.length,
  });
}

async function handlePostSession(req, res) {
  const payload = await readJsonBody(req);
  const envelope = normalizeIncomingPayload(payload);
  const dedupKey = dedupKeyFor(envelope);

  const existing = dedupMap.get(dedupKey);
  if (existing) {
    sendJson(res, 200, {
      ok: true,
      duplicate: true,
      ingestionId: existing.ingestionId,
      sessionId: existing.session.id,
      receivedAt: existing.receivedAt,
    });
    return;
  }

  const record = {
    ingestionId: randomUUID(),
    dedupKey,
    receivedAt: new Date().toISOString(),
    ...envelope,
  };

  await appendRecord(record);
  sessions.push(record);
  dedupMap.set(dedupKey, record);

  sendJson(res, 201, {
    ok: true,
    duplicate: false,
    ingestionId: record.ingestionId,
    sessionId: record.session.id,
    receivedAt: record.receivedAt,
  });
}

function handleListSessions(req, res, url) {
  const queryFilters = parseQueryFilters(url.searchParams);
  const limit = parseLimitOrDefault(url.searchParams.get("limit"), 50, 1, 500);
  const offset = parseLimitOrDefault(url.searchParams.get("offset"), 0, 0, 1_000_000);

  const filtered = applyRecordFilters(sessions, queryFilters).sort(
    (a, b) => Date.parse(b.receivedAt) - Date.parse(a.receivedAt)
  );

  const page = filtered.slice(offset, offset + limit).map(toSessionSummary);

  sendJson(res, 200, {
    ok: true,
    total: filtered.length,
    limit,
    offset,
    items: page,
  });
}

function handleGetSessionById(res, sessionRef) {
  const id = decodeURIComponent(sessionRef || "").trim();
  if (!id) throw httpError(400, "ID de sessao invalido.");

  const record = sessions.find((item) => item.ingestionId === id || item.session.id === id);
  if (!record) throw httpError(404, "Sessao nao encontrada.");

  sendJson(res, 200, {
    ok: true,
    item: record,
  });
}

function handleGetStats(res, url) {
  const queryFilters = parseQueryFilters(url.searchParams);
  const filtered = applyRecordFilters(sessions, queryFilters);

  sendJson(res, 200, {
    ok: true,
    stats: buildStats(filtered),
  });
}

const server = createServer(async (req, res) => {
  withCors(res);

  if (req.method === "OPTIONS") {
    res.writeHead(204);
    res.end();
    return;
  }

  const url = requestUrl(req);
  const pathname = url.pathname.endsWith("/") && url.pathname !== "/"
    ? url.pathname.slice(0, -1)
    : url.pathname;

  try {
    if (pathname === "/health" && req.method === "GET") {
      handleGetHealth(res);
      return;
    }

    if (pathname.startsWith("/filtertrack/") && !isAuthorized(req)) {
      sendError(res, 401, "Nao autorizado. Header X-API-Key ausente ou invalido.");
      return;
    }

    if (pathname === "/filtertrack/sessions" && req.method === "POST") {
      await handlePostSession(req, res);
      return;
    }

    if (pathname === "/filtertrack/sessions" && req.method === "GET") {
      handleListSessions(req, res, url);
      return;
    }

    if (pathname.startsWith("/filtertrack/sessions/") && req.method === "GET") {
      const sessionRef = pathname.slice("/filtertrack/sessions/".length);
      handleGetSessionById(res, sessionRef);
      return;
    }

    if (pathname === "/filtertrack/stats" && req.method === "GET") {
      handleGetStats(res, url);
      return;
    }

    sendError(res, 404, "Rota nao encontrada.");
  } catch (err) {
    const statusCode =
      err && Number.isInteger(err.statusCode) && err.statusCode >= 400
        ? err.statusCode
        : 500;

    if (statusCode >= 500) {
      console.error("[server] erro interno", err);
    }

    sendError(res, statusCode, statusCode >= 500 ? "Erro interno do servidor." : err.message);
  }
});

async function start() {
  await loadStore();

  server.listen(PORT, HOST, () => {
    console.log(`[filtertrack-backend] listening on http://${HOST}:${PORT}`);
    console.log(`[filtertrack-backend] data file: ${DATA_FILE}`);
    console.log(`[filtertrack-backend] sessions loaded: ${sessions.length}`);
    if (API_KEY) {
      console.log("[filtertrack-backend] API key protection: enabled");
    } else {
      console.log("[filtertrack-backend] API key protection: disabled");
    }
  });
}

start().catch((err) => {
  console.error("[filtertrack-backend] failed to start", err);
  process.exitCode = 1;
});
