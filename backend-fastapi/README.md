# FilterTrack Backend

Backend minimal em **FastAPI + SQLite**, pronto para deploy no Fly.io usando um volume local em vez de Managed Postgres.

## Endpoints

- `POST /filtertrack/sessions`
- `GET /filtertrack/sessions`
- `GET /filtertrack/sessions/:id`
- `GET /filtertrack/stats`
- `GET /health`

Compatibilidade com o app Android atual:

- Endpoint de ingestao no app deve ser: `https://<seu-app>.fly.dev/filtertrack/sessions`
- Payload aceito no formato que o `index.html` ja envia.

## Rodar local

```bash
cd backend-fastapi
python -m venv .venv
. .venv/Scripts/activate   # Windows PowerShell: .\.venv\Scripts\Activate.ps1
pip install -r requirements.txt
uvicorn app.main:app --host 0.0.0.0 --port 8080 --reload
```

Sem configuracao extra, o banco local fica em `./filtertrack.db`.

Health check:

```bash
curl http://127.0.0.1:8080/health
```

## Variaveis de ambiente

- `DATABASE_PATH` (padrao local `./filtertrack.db`; no Fly `/data/filtertrack.db`)
- `DATABASE_URL` (opcional, somente URLs `sqlite:///...`)
- `HOST` (padrao `0.0.0.0`)
- `PORT` (padrao `8080`)
- `MAX_SAMPLES_PER_SESSION` (padrao `120000`)
- `CORS_ALLOW_ORIGINS` (CSV, padrao `*`)
- `FILTERTRACK_API_KEY` (opcional)
- `FILTERTRACK_USER_KEY` (chave para usuarios comuns acessarem o BI)
- `FILTERTRACK_ADMIN_KEY` (chave para administradores aprovarem alteracoes)

Se `FILTERTRACK_API_KEY` for definida, as rotas `/filtertrack/*` exigem header `X-API-Key`.

## BI web

O painel web fica em:

```text
https://filtertrack-api.fly.dev/bi
```

Niveis de acesso:

- Usuario comum: ve dados, graficos, detalhes, downloads e envia propostas de edicao/filtros.
- Administrador: faz tudo do usuario comum e tambem aprova/rejeita propostas.

Headers usados pelo BI:

```text
X-Access-Key: <FILTERTRACK_USER_KEY ou FILTERTRACK_ADMIN_KEY>
```

Downloads disponiveis no painel:

- sessoes em CSV,
- amostras em CSV,
- sessoes completas em JSON.

Graficos principais:

- sessoes e amostras ao longo do tempo,
- amostras por dispositivo, filtro e estacao,
- vazao media liquida por filtro em L/min,
- direcao da vazao por filtro,
- tendencia diaria de vazao liquida e absoluta.

As propostas de edicao nao alteram os dados imediatamente. Elas ficam em `change_proposals` ate um administrador aprovar. Filtros customizados aprovados ficam em `approved_filters`.

## Contrato de armazenamento

O backend aceita o payload do app Android, mas grava uma versao canonica e enxuta:

- Mantem metadados da sessao, dispositivo e filtro.
- Mantem metadados do filtro que nao sao calculados: `id`, `name`, `areaM2`, `station`, `location`, `businessUnit`, `custom`, `createdAt`.
- Mantem amostras como dados de origem em formato compacto:
  - `samples.format = "distance_cm_x100_v1"`
  - `samples.t0` = timestamp Unix em milissegundos da primeira amostra
  - `samples.t` = offsets em milissegundos desde `t0`
  - `samples.d` = distancia em centimetros multiplicada por `100`
- Remove campos calculaveis das amostras antes de gravar: `isoTime`, `flow10sLpm`, `flow60sLpm`, `flowSessionLpm`, `direction`.
- Remove `sampleCount` do JSON salvo; o total e recalculado a partir de `samples` e armazenado em coluna operacional para listagem/estatisticas.

Exemplo compacto:

```json
{
  "format": "distance_cm_x100_v1",
  "t0": 1770000000000,
  "dtUnit": "ms",
  "distanceUnit": "cm",
  "scale": 100,
  "t": [0, 100, 200],
  "d": [2428, 2429, 2430]
}
```

Nesse exemplo, `d: 2428` significa `24.28 cm`.

## Deploy no Fly.io com SQLite em volume

O `fly.toml` monta o volume `filtertrack_data` em `/data` e configura `DATABASE_PATH=/data/filtertrack.db`.

1. Login no Fly:

```bash
fly auth login
```

2. Criar o app se ele ainda nao existir:

```bash
fly apps create filtertrack-api --org personal
```

Se usar outro nome de app, atualize `app = "..."` no `fly.toml`.

3. Criar um volume pequeno na mesma regiao do app:

```bash
fly volumes create filtertrack_data --region gru --size 1 -a filtertrack-api
```

4. Garantir que nao existe secret antigo de Postgres:

```bash
fly secrets unset DATABASE_URL -a filtertrack-api
```

5. Opcionalmente definir chave de API:

```bash
fly secrets set FILTERTRACK_API_KEY="<chave-forte>" -a filtertrack-api
```

6. Deploy:

```bash
fly deploy -a filtertrack-api
```

7. Verificar:

```bash
fly status -a filtertrack-api
fly logs -a filtertrack-api
curl https://filtertrack-api.fly.dev/health
```

## Limites desta configuracao

- Use uma maquina do app. Volumes do Fly sao locais a uma maquina e nao sao armazenamento compartilhado.
- O `fly.toml` usa `min_machines_running = 0` para reduzir custo quando nao ha trafego.
- Faca backup do arquivo `/data/filtertrack.db` se os dados forem importantes.
- Migre para Postgres se precisar de varias maquinas escrevendo no mesmo banco, alta disponibilidade ou recuperacao gerenciada.

## Parametros de consulta

`GET /filtertrack/sessions` e `GET /filtertrack/stats` aceitam:

- `from=<ISO-8601>`
- `to=<ISO-8601>`
- `deviceAddress=<mac>`
- `filterId=<id>`

Exemplo:

```bash
curl "https://filtertrack-api.fly.dev/filtertrack/sessions?limit=20&from=2026-04-01T00:00:00Z"
```

## Observacao sobre o app Android

No app, configure o `serverUrl` para:

```text
https://filtertrack-api.fly.dev/filtertrack/sessions
```

Se ativar `FILTERTRACK_API_KEY`, sera necessario ajustar o app para enviar `X-API-Key` nas requisicoes `fetch`.
