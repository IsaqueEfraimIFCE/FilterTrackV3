# FilterTrack Backend

Backend minimo para receber as sessoes geradas pelo app e consultar historico.

## O que foi implementado

- `POST /filtertrack/sessions` para ingestao de sessoes (payload do app).
- Deduplicacao por `app + session.id` (idempotencia para reenvios).
- Persistencia em `NDJSON` em `backend/data/sessions.ndjson`.
- `GET /filtertrack/sessions` para listagem com filtros.
- `GET /filtertrack/sessions/:id` para detalhe da sessao.
- `GET /filtertrack/stats` para metricas agregadas.
- `GET /health` para health check.

## Execucao local

Opcao rapida (Windows PowerShell):

```bash
cd backend
./run-local.ps1
```

Opcao rapida (Windows cmd.exe):

```bash
cd backend
run-local.bat
```

Comando direto:

```bash
cd backend
npm start
```

Modo desenvolvimento (auto-reload):

```bash
cd backend
npm run dev
```

O script `run-local.ps1` imprime os endpoints prontos para:

- emulador Android (`10.0.2.2`)
- dispositivo fisico na mesma rede (IP local da maquina)
- health check local (`http://127.0.0.1:8080/health`)

Variaveis de ambiente (opcionais):

- `HOST` (padrao: `0.0.0.0`)
- `PORT` (padrao: `8080`)
- `DATA_DIR` (padrao: `./data`)
- `MAX_PAYLOAD_MB` (padrao: `4`)
- `MAX_SAMPLES_PER_SESSION` (padrao: `120000`)
- `FILTERTRACK_API_KEY` (se definido, exige `X-API-Key`)
- `CORS_ALLOW_ORIGIN` (padrao: `*`)

Copie `.env.example` para `.env` e exporte as variaveis no seu ambiente, se desejar.

## Endpoint para o app

No app, o `serverUrl` deve apontar para:

```text
http://<host>:8080/filtertrack/sessions
```

Exemplos:

- Emulador Android: `http://10.0.2.2:8080/filtertrack/sessions`
- Dispositivo fisico na mesma rede: `http://SEU_IP_LOCAL:8080/filtertrack/sessions`

## Exemplos de chamada

Ingestao:

```bash
curl -X POST http://localhost:8080/filtertrack/sessions \
  -H "Content-Type: application/json" \
  -d '{
    "schemaVersion": 1,
    "app": "FilterTrack",
    "uploadedAt": "2026-04-24T12:00:00.000Z",
    "session": {
      "id": "session-123",
      "startedAt": "2026-04-24T11:59:00.000Z",
      "endedAt": "2026-04-24T12:00:00.000Z",
      "sampleCount": 1,
      "device": { "name": "filtertrack-01", "address": "AA:BB:CC:DD:EE:FF" },
      "filter": { "id": "filtro-1", "name": "Filtro A", "areaM2": 2.5 },
      "samples": [{ "ts": 1713960000000, "distanceCm": 54.2 }]
    }
  }'
```

Listagem:

```bash
curl "http://localhost:8080/filtertrack/sessions?limit=20&from=2026-04-01T00:00:00Z"
```

Stats:

```bash
curl "http://localhost:8080/filtertrack/stats"
```

## Opcoes de evolucao (arquitetura)

1. Atual (implementada): Node nativo + arquivo NDJSON.
   Uso recomendado: piloto, baixa/midia volumetria e deploy rapido.
2. FastAPI + PostgreSQL.
   Uso recomendado: validacoes mais ricas, analytics SQL e escala moderada.
3. Supabase (Postgres gerenciado + Auth + Storage).
   Uso recomendado: menos operacao de infra e backend produtivo com baixo esforço.
