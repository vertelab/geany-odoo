# geany-pi

An **OpenAI-compatible** coding assistant as a **native C plugin** for
**Geany 2.x** (GTK 3).

Ask an LLM (via any `/v1/chat/completions` endpoint such as
[Vertel bifrost](http://192.168.11.150:8080/v1), llama.cpp, Ollama or OpenAI
itself) to modify the active Geany document — or only the current selection.
The reply is shown for review before it is applied, and the replacement is a
**single undoable** Scintilla edit (one `Ctrl+Z` returns to the previous
content).

## Features

- **Scope control** — ask about the whole document *or* only the selection.
- **All providers supported** — just configure base-URL + optional API key +
  model. Defaults point at the Vertel bifrost OpenAI-compatible gateway.
- **Safe apply flow** — reply appears in an editable preview. Choose
  *Apply* / *Try again* / *Discard*. Applying replaces the original region in
  one undo-friendly operation.
- **Async HTTP** — network happens in a worker thread; the editor never
  blocks. Timeout configurable.
- **Per-user config** saved to
  `~/.config/geany/geany-pi/geany-llm.conf` (private → `chmod 600`).

## Build

Requirements (Ubuntu 24.04 · Geany 2.0):

```bash
sudo apt install build-essential libgtk-3-dev libglib2.0-dev \
                 libjson-glib-dev libcurl4-openssl-dev pkg-config
```

Build:

```bash
make            # produces ./geany-pi.so
make install    # copies into ~/.config/geany/plugins/ and ~/.local/share/geany/plugins
```

Geany (re)discovers new plugins via **Tools ▸ Plugin Manager**. Enable
**geany-llm**, then use **Tools ▸ Ask LLM** (or the ✔ selection-aware
submenu).

## Configuration

Open **Tools ▸ geany-llm Settings…**

| Field           | Meaning                                           | Default                      |
|-----------------|---------------------------------------------------|------------------------------|
| Base URL        | OpenAI-compatible root, e.g. `http://host:8080/v1`| `http://192.168.11.150:8080/v1` |
| API key         | Bearer token (optional, empty = unauthenticated)  | *(empty)*                    |
| Model           | model id reported to the endpoint                 | `qwen3-coder`                |
| System prompt   | system-role instruction sent with every request   | ask to reply code only       |
| Timeout (s)     | libcurl network timeout                            | `120`                        |

Config file layout (GLib GKeyFile):

```ini
[provider]
base_url = http://192.168.11.150:8080/v1
api_key =
model = qwen3-coder

[behavior]
system_prompt = …
timeout_sec = 120
ask_before_apply = true
```

## How the request is built

1. `Tools ▸ Ask LLM` → dialog: **Affect** (whole document / selection) and a
   free-text instruction field.
2. `Send` snapshots the current editor region and background-worker calls
   `POST {base_url}/chat/completions` with
   `{model, messages:[{system},{user:[instruction + source code]}], temperature:0}`.
3. The assistant reply is shown in the result dialog. You may edit it before
   applying.
4. **Apply** replaces the region with one SciReplaceSel call. **Try again**
   re-runs the same job, **Discard** drops it.

## Source layout

```
src/
├── geany-llm.c         main entry, GeanyPlugin registration, Tools menu
├── geany-llm-common.h  shared structs (GemmPrefs, GemmRequestKind, APIs)
├── active.c            Scintilla/doc helpers (get text, replace region)
├── prefs.c             GKeyFile config load/save + settings dialog
├── http.c              libcurl client + json-glib (worker-thread safe)
└── request.c           async flow + preview/apply dialogs
```

## Notes / roadmap

- Reading/writing happens on the **raw code in the editor** — the plugin does
  **not** interact with the `odoo-pfiles` `preprocess` `-D VERSION` flow in
  v0. Version expansion/round-tripping of `.p.` files is a future layer.
- No gettext catalog yet; UI strings are plain English (the `_()` macro is an
  identity in Geany without `main_locale_init`).
- API level guard: `PLUGIN_VERSION_CHECK(224)` (Geany 2.0) & registration
  through `GEANY_PLUGIN_REGISTER(plugin, 224)`.

## License

GPL-2.0-or-later (in line with Geany's own licensing).

---

# Agent integration (Pi in Geany)

The plugin is not just a chat panel: it exposes a **control bridge** so an
external agent running in a terminal (e.g. **Pi**) can read and modify the
active Geany document. The plugin is only the sensor/actuator — the agent
does the reasoning with its own tools and skills.

```
┌─ Geany ────────────────────────────────────────────────┐
│  geany-llm plugin                                      │
│    ├── bridge.c    ──► $XDG_RUNTIME_DIR/geany-pi.sock │
│    │                   (JSON per rad)                  │
│    └── terminal.c  ──► VTE-flik "Pi" (kör `pi`)        │
└────────────────────────────────────────────────────────┘
                 ▲  unix-socket
                 │
        geany-pi-ctl   ◄── agentens bash-verktyg
```

## Embedded terminal (optional)

The "Pi" tab needs the VTE development files at build time:

```bash
sudo apt install libvte-2.91-dev
cd ~/geany-llm && make clean && make && make install
```

Without VTE the menu item still exists but only prints a status message.

## CLI: `geany-pi-ctl`

Installed to `~/.local/bin/geany-pi-ctl`.

```bash
geany-pi-ctl ping                 # är bryggan uppe?
geany-pi-ctl info                 # JSON: fil, språk, markering, cursor
geany-pi-ctl docs                 # öppna dokument
geany-pi-ctl get-document         # hela bufferten till stdout
geany-pi-ctl get-selection        # aktuell markering till stdout
geany-pi-ctl select 10 40         # sätt markering (start, end)
geany-pi-ctl set-document --file new.py
printf 'xyz' | geany-pi-ctl set-selection
printf 'xyz' | geany-pi-ctl insert
geany-pi-ctl status "Pi: tittar på koden"
geany-pi-ctl open /path/to/file.py
```

### Viktigt om text och nyrader

`<<<` (bash here-string) lägger till en avslutande nyrad. Vill du skriva exakt:

```bash
printf 'exact text' | geany-pi-ctl set-selection
# eller
geany-pi-ctl set-document --file /tmp/new.py
```

## Exempel: agent-loop

```bash
# 1. läs koden agenten ska arbeta med
geany-pi-ctl get-selection > /tmp/sel.py

# 2. ... agenten bearbetar /tmp/sel.py ...

# 3. skriv tillbaka (ersätter markeringen, eller hela dokumentet om
#    ingen markering fanns — den nya texten hamnar i editorn)
geany-pi-ctl set-selection --file /tmp/sel.new
```

## Protokoll (för egna klienter)

Ett JSON-objekt per rad, newline-terminerat, över
`$XDG_RUNTIME_DIR/geany-pi.sock` (mode 0600).

| cmd | beskrivning |
|---|---|
| `ping` | `{ok,plugin,version,pid}` |
| `info` | `{ok,file,language,has_selection,selection_start,selection_end,length,cursor,line}` |
| `get` | `{cmd:"get",scope:"document"\|"selection"}` → `{ok,text,...}` |
| `set` | `{cmd:"set",scope:...,text:"..."}` → `{ok,applied}` |
| `insert` | `{cmd:"insert",text:"..."}` → `{ok,at}` |
| `select` | `{cmd:"select",start:N,end:M}` → `{ok}` |
| `status` | `{cmd:"status",text:"..."}` → skriver i Geany's Status-flik |
| `docs` | `{ok,docs:[{file,current}]}` |
| `open` | `{cmd:"open",file:"/abs/path"}` → `{ok,file}` |
| `codes` | debug: `{start,len}` → `{ok,length,codes:[...]}` |

---

# Geany version compatibility

Verified against **Geany 2.0** (Ubuntu 24.04) and **Geany 2.1**
(released 2025-07-06), source-level.

| | Geany 2.0 | Geany 2.1 |
|---|---|---|
| `GEANY_API_VERSION` | 247 | 250 |
| `GEANY_ABI_VERSION` | `73 << 8` | `73 << 8` |
| Classic plugin contract (`plugin_init`, …) | yes | yes (deprecated, still supported) |
| `scintilla_send_message` / unprefixed Scintilla aliases | – | removed, not used by us |
| our plugin compiles | ✓ | ✓ (all 8 files) |

Notes:

- The **ABI is unchanged** between 2.0 and 2.1, so a plugin built for 2.0
  passes 2.1's ABI guard too. Rebuilding against 2.1 headers is still the
  recommended practice.
- `PLUGIN_VERSION_CHECK(224)` declares a **minimum** API (224), which is fine
  on both 247 and 250.
- This plugin uses the **classic entry points** on purpose: the Ubuntu 2.0
  build loads `plugin_init`/`plugin_set_info`/`plugin_version_check`/
  `plugin_configure`. Geany 2.1 still supports these (only the newer
  `geany_load_module` registration is recommended for new plugins).
- Geany 2.1 bumps Scintilla to 5.5.4. The released geany-plugins needed fixes
  around `sci_get_contents` / `SCI_GETTEXT` / `sci_get_selected_text` — that is
  exactly the API family this plugin already avoids (we use
  `sci_get_contents_range`), which is why document reads are byte-exact.

## Building for a specific Geany version

The build uses the installed Geany headers via `pkg-config geany`, so building
on a 2.1 system produces a 2.1 plugin automatically.

To compile-check against another Geany release without installing it:

```bash
tar xjf geany-2.1.tar.bz2 && cd geany-2.1
PKG=$(pkg-config --cflags gtk+-3.0 glib-2.0 json-glib-1.0)
INC="-DGTK -Iplugins -Isrc -Iscintilla/include -Iscintilla/lexilla/include -Isrc/tagmanager"
for f in ~/geany-pi/src/*.c; do
  cc -c -O2 -fPIC -Wall $INC $PKG -o /tmp/o.o "$f" || echo "FAIL $f"
done
```

(`-DGTK` is required: `ScintillaWidget.h` only defines `ScintillaObject`
under `#if defined(GTK)`. `pkg-config geany` emits it automatically.)

## Availability

- Ubuntu 24.04 (noble): Geany 2.0
- Ubuntu 25.04 (plucky): Geany 2.0-2
- Geany 2.1: Debian sid / Ubuntu 25.10+, or a PPA, or built from source

---

# Pi MCP integration (`ai_pi_mcp`)

`ai_pi_mcp` is the Vertel **Odoo MCP developer server**: a standalone
streamable-HTTP MCP endpoint (`POST /mcp`, protocol revision `2025-03-26`) that
lets a coding agent edit `ir.ui.view` archs, install/upgrade modules,
introspect models and fix data directly against the Odoo database. It
authenticates with the caller's normal Odoo API key (Bearer).

The plugin configures Pi for **the machine Geany is running on** (i.e. the
machine that has the document open), because that is where the Pi agent runs.

## What the plugin does

On `plugin_init` (and via **Tools ▸ Setup Pi MCP (this machine)**) it:

1. reads `~/.pi/agent/odoo.json` (`baseUrl`, `token`),
2. derives the MCP endpoint (`<baseUrl>` minus `/ai/v1` + `/mcp`),
3. merges an entry into `~/.pi/agent/mcp.json` — idempotent, every other
   server is preserved:

```json
{
  "mcpServers": {
    "ai_pi_mcp": {
      "transport": "streamable-http",
      "url": "https://ledningssystem.vertel.se/mcp",
      "headers": { "Authorization": "Bearer <odoo api key>" },
      "lifecycle": "lazy"
    }
  }
}
```

Pi exposes the tools as `mcp__ai_pi_mcp__<tool>`.

Override the derived URL (e.g. to force a locally reachable Odoo) with:

```bash
export GEANY_LLM_PI_MCP_URL=http://localhost:8069/mcp
```

If `~/.pi/agent/odoo.json` is absent the setup is skipped silently
(status line only) — machines without an Odoo connection are unaffected.

## Tools exposed by `ai_pi_mcp`

`list_models`, `describe_model`, `whoami`, `get_access_rights`,
`view_list`, `view_get_arch`, `view_get_merged`, `view_set_arch`,
`view_restore`, `action_list`, `action_set_domain`, `action_set_view_mode`,
`action_set_order`, `module_list`, `module_install`, `module_uninstall`,
`module_upgrade`, `search_read`, `count`, `read_records`, `create_record`,
`update_record`, `delete_record`, `call_method`

## Combined workflow (Geany + Pi + MCP)

```bash
# in Geany's "Pi" terminal tab, on the machine with the document open:
geany-pi-ctl get-selection          # read the code from the editor
# ... ask Pi to work on it; Pi may also use mcp__ai_pi_mcp__* tools
geany-pi-ctl set-selection --file /tmp/out.py   # write changes back
```

So Pi can edit **both** the local code in Geany (via the bridge) **and** the
Odoo database (via `ai_pi_mcp`) from the same terminal.

---

# MCP for documents that live on *other* machines

Geany is typically run on one machine but opens Odoo source **over gvfs/sftp**
from many different machines:

```
/run/user/1000/gvfs/sftp:host=192.168.11.145/usr/share/odooext-se_sfa/.../x.py
/run/user/1000/gvfs/sftp:host=192.168.11.33/usr/share/.../y.py
```

For real Odoo development on such a file, the useful MCP server is **that
machine's own** `ai_pi_mcp` (`http://<host>:8069/mcp`) — not the central one.

## What the plugin does

`scripts/geany-pi-mcp-sync` (installed as `~/.local/bin/geany-pi-mcp-sync`) is invoked by
the plugin at startup and from **Tools ▸ Sync Pi MCP for open documents**.
It

1. collects the distinct `sftp:host=<H>` values from the open documents,
2. resolves a URL + token for each host (see below),
3. merges one MCP entry per host into `~/.pi/agent/mcp.json`:

```json
"ai_pi_mcp_192_168_11_145": {
  "transport": "streamable-http",
  "url": "http://192.168.11.145:8069/mcp",
  "headers": { "Authorization": "Bearer <token>" },
  "lifecycle": "lazy"
}
```

(the local machine keeps the plain `ai_pi_mcp` entry). Pi exposes them as
`mcp__ai_pi_mcp_192_168_11_145__view_set_arch` etc.

## Requirements per machine

| # | Requirement | Notes |
|---|---|---|
| 1 | `ai_pi_mcp` installed and `ai_pi_mcp.enabled=True` | checked via `GET /mcp` → 405 means the route exists, 404 means the module is missing |
| 2 | a Bearer token **valid in that machine's database** | a per-user Odoo API key from *that* DB, or `ai_pi_mcp.developer_user_id` + the shared `ai_pi_mcp.api_key` |
| 3 | a token entry for the host (see below) | or the host's own `~/.pi/agent/odoo.json` when its `baseUrl` points at that host |

Token resolution order (per host):

1. `~/.config/geany/geany-pi/mcp-hosts.json` → `hosts.<host>.{url,token}`
2. `~/.config/geany/geany-pi/mcp-hosts.json` → `default_token`
   (+ optional `default_url_pattern`, default `http://{host}:8069/mcp`)
3. the host's `~/.pi/agent/odoo.json` read over ssh — only if its `baseUrl`
   contains the host
4. otherwise the host is skipped and reported

## Configuration example

`~/.config/geany/geany-pi/mcp-hosts.json`:

```json
{
  "default_token": "<one dev key reused on every machine>",
  "hosts": {
    "192.168.11.145": { "token": "<key valid in sfa-test>" },
    "192.168.11.33":  { "url": "http://192.168.11.33:8069/mcp",
                        "token": "<key valid in svenskfast>" }
  }
}
```

Then run **Tools ▸ Sync Pi MCP for open documents** (or just restart Geany)
and restart Pi so it picks up the new servers:

```bash
geany-pi-mcp-sync --host 192.168.11.145 --dry-run   # see what would be written
cat ~/.config/geany/geany-pi/mcp-sync.log    # what the plugin last did
```

## Known state (2026-09-10)

| Host | `/mcp` | Note |
|---|---|---|
| 192.168.11.91 (ledningssystem) | 405 → present | works with the central key |
| 192.168.11.145 (sfa18-test, db `sfa-test`) | 405 → present | needs a token valid in `sfa-test`; the central key gives **401 Invalid API key** |
| 192.168.11.33 (svenskfast) | 404 → **missing** | `ai_pi_mcp` is not installed there |

---

# Token strategy for many machines (smart setup)

Problem: every machine that runs `ai_pi_mcp` has *its own* database, and an Odoo
API key is only valid in the database it was created in. With new machines
appearing all the time, per-machine keys do not scale.

## The lever: one shared token can be valid everywhere

From `ai_pi_mcp`'s `_check_auth()`:

```python
caller_uid = res.users.apikeys._check_credentials(scope='rpc', key=token)
dev_user_id = param('ai_pi_mcp.developer_user_id')
if dev_user_id:
    shared = param('ai_pi_mcp.api_key', '')
    if not caller_uid and (not shared or token != shared):
        raise AccessError('Invalid API key')
    return browse(dev_user_id)          # acts as the service account
```

* Both mechanisms work **simultaneously** — a caller's own per-user API key is
  always accepted; the shared key is an *additional* accepted token.
* The shared key is compared as **plain text**, and it is just an
  `ir.config_parameter` value — so **we choose it**.
* Therefore: set the *same* `ai_pi_mcp.developer_user_id` + `ai_pi_mcp.api_key`
  on every machine and **one token works everywhere**.

## Recommended: one pillar secret, two Salt states

```
pillar/pi_mcp.sls
    pi_mcp:
      shared_token: !!! secret !!!
      developer_login: ai-dev        # service account on dev DBs
      url_pattern: http://{host}:8069/mcp
```

**On every Odoo dev machine** (`odoo-ai.pi_mcp`):

```sls
ai_pi_mcp.params:                    # idempotent upsert
  cmd.run:
    - name: |
        psql -d {{ db }} -v ON_ERROR_STOP=1 <<'SQL'
        INSERT INTO ir_config_parameter (key, value) VALUES
          ('ai_pi_mcp.enabled', 'True'),
          ('ai_pi_mcp.api_key', '{{ pillar.pi_mcp.shared_token }}'),
          ('ai_pi_mcp.developer_user_id',
             (SELECT id::text FROM res_users WHERE login='{{ pillar.pi_mcp.developer_login }}'))
        ON CONFLICT (key) DO UPDATE SET value = EXCLUDED.value;
        SQL
```

**On every workstation** — no per-machine secrets, just the shared token:

```sls
/etc/geany-pi/mcp-hosts.json:
  file.managed:
    - contents: |
        { "default_token": "{{ pillar.pi_mcp.shared_token }}",
          "default_url_pattern": "{{ pillar.pi_mcp.url_pattern }}" }
    - makedirs: True
    - mode: '0644'
```

`geany-pi-mcp-sync` reads, in order: `$GEANY_LLM_MCP_HOSTS`,
`/etc/geany-pi/mcp-hosts.json`, `~/.config/geany/geany-pi/mcp-hosts.json`.

## Result

| Event | Work needed |
|---|---|
| New dev machine | add it to Salt (one state) — nothing on workstations |
| New workstation | add it to Salt (one state) — nothing per machine |
| Rotate the token | change one pillar value, re-apply |
| New document host | **nothing** — the URL pattern covers any host |

## Alternatives

* **Runtime fetch (zero secrets at rest).** The workstation already has SSH to
  every machine; the helper can read that machine's `ai_pi_mcp.api_key`
  (`odoo shell` / `psql` over ssh) and cache it briefly. Nothing is stored, but
  it needs DB/shell access and adds latency.
* **Per-user API keys** (best audit trail: `whoami` shows the human). Distribute
  them the same way via Salt if you want attribution instead of one `ai-dev`
  identity.
* **Router (no Pi restarts).** A small local MCP proxy in the plugin that
  targets the *active document's* machine. Pi registers one server
  (`http://127.0.0.1:PORT/mcp`) and never needs a restart when a new host
  appears.

## Security notes

* `ai_pi_mcp.enabled=True` is already a dev-only opt-in — keep it off on
  production DBs.
* Use `ai_pi_mcp.read_only=True` where writes are not needed.
* Do **not** expose `/mcp` publicly; reach it over the internal network.
* Prefer a dedicated `ai-dev` service account over a personal one (no personal
  key rotation, clear audit trail).
* The shared token grants `call_method`/writes — treat it as a credential and
  rotate it from the single pillar.

---

# Terminal tab (one terminal per document directory)

Geany 2.1 from the PPA does **not** link VTE (`ldd /usr/bin/geany` shows no
`libvte`), so there is no built-in terminal. The plugin provides its own
**Terminal** tab in the message window:

* An inner notebook holds **one terminal per directory** (not per file — files
  in the same directory share a terminal).
* Sessions **follow the active document** (`document-activate`) and are
  **closed when the last document in that directory is closed**
  (`document-close`).
* At most **6** sessions; the least recently used one (not used by an open
  document) is closed when a new directory needs a terminal.
* Tabs are labelled `host:dir` (host kept intact, long directories shortened)
  with the full location as tooltip; each tab has a `×` to close it.

## Behaviour by document type

| Document path | Terminal |
|---|---|
| local, e.g. `/home/u/src/x.py` | program with `cwd` = the file's directory |
| gvfs/sftp, e.g. `/run/user/1000/gvfs/sftp:host=192.168.11.145/usr/share/…/x.py` | `ssh -t 192.168.11.145 "cd '/usr/share/…' && exec <program>"` |

Each terminal runs the user's **login shell** (`$SHELL -l`). The terminal is a
plain terminal and is deliberately **independent of Pi / agent tooling**;
`$GEANY_LLM_TERMINAL` can override which program is started.

The gvfs **mount is not used** for the terminal — only host and path are parsed
out of it, so ssh works even if the mount is stale. Supported path form:

```
/gvfs/<scheme>:host=<host>[,user=<u>][,port=<p>]/<abs/path>
```

Anything without `/gvfs/` is treated as a local path.

If the remote directory does not exist the shell falls back to `$HOME` after
printing a notice.

## Notes

* Requires `libvte-2.91-dev` at build time (`-DHAVE_VTE` is set by the
  Makefile when `pkg-config vte-2.91` resolves).
* SSH uses the same keys/agent as gvfs, so hosts that already work over sftp
  work here; the first connection may still ask about the host key.
* **Avoid globbing over gvfs paths** (`ls /run/user/…/gvfs/…/*/*.py`) — it can
  block for a long time; use a known path or `stat`.

---

# Tabs and the document's machine (v0.2)

Two independent tabs in Geany's message window, plus an agent control surface:

| Tab | What it is |
|---|---|
| **Pi** | one Pi coding-agent session (`pi`), started once. Separate from the terminal. |
| **Multiterm** | per-directory `$SHELL` terminals; remote (gvfs/sftp) documents get `ssh -t <host> "cd <dir> && exec $SHELL -l"`. |

Menu: **Pi**, **Terminal**, **Setup Pi MCP (this machine)**,
**Sync Pi MCP for open documents**.

## `geany-pi-git` — git where the document lives

Geany can hold documents from other machines. Committing and pushing such a
file must happen **on that machine**, in the repository that contains it:

```bash
geany-pi-git info            # file, remote/host/dir, changed
geany-pi-git root            # repository root (on the right machine)
geany-pi-git status | diff [--staged] | log -n 15 | branch | remote
geany-pi-git add <path>...
geany-pi-git commit -m "msg" [--all]
geany-pi-git push [--set-upstream]
geany-pi-git raw <git args>  # advanced
```

It resolves the target from the bridge (`geany-pi-ctl info`, which now returns
`remote`, `host`, `user`, `port`, `dir`), then runs git locally or via
`ssh <host>`. Exit code mirrors git's.

Verified: `commit` on a local repo, and `git ls-remote`/push auth from
192.168.11.145 (github `vertelab/odoo-account`, branch `18.0`; that machine has
its own id_ed25519, so no agent forwarding is needed).

## Source layout (v0.2)

```
src/
├── geany-llm.c         registration, menus, lifecycle
├── geany-llm-common.h  shared declarations
├── log.c               gemm_logf (append-only debug log)
├── active.c            document/editor helpers (doc-parameterised)
├── bridge.c            UNIX-socket bridge (+ remote/host/dir in `info`)
├── pi_mcp.c            ai_pi_mcp config, router, per-machine sync
├── terminal.c          per-directory $SHELL terminals
└── pi_tab.c            single Pi agent tab
scripts/
├── geany-pi-ctl       bridge CLI
├── geany-pi-git       git at the document's machine
├── geany-pi-mcp-sync         Pi MCP config for the open documents' machines
└── geany-pi-mcp-router       local MCP router (follows the active document)
```

The old bifrost/LLM-panel code (http.c, request.c, llm_panel.c, prefs.c) has
been removed; `~/.config/geany/geany-pi/geany-llm.conf` is no longer used.

---

# geany-pi 0.3 (omdöpning + konfiguration via Tillägg)

* Tillägget heter nu **geany-pi** (`geany-pi.so`), hjälparna `geany-pi-ctl`,
  `geany-pi-git`, `geany-pi-mcp-sync`, `geany-pi-mcp-router`. Socket:
  `$XDG_RUNTIME_DIR/geany-pi.sock`. Konfig/logg: `~/.config/geany/geany-pi/`.
* Terminal-fliken heter **Multiterm**.
* **All konfiguration sker i Geanys Pluginhanterare → Inställningar**
  (`plugin_configure`), inte via menyval under Verktyg — menyvalen är borttagna.
  Där finns:
  * Multiterm: program (tomt = `$SHELL -l`), max samtidiga terminaler
  * Pi: program
  * MCP: delad token, URL-mönster, samt knapparna *Skriv mcp-hosts.json*,
    *Sätt upp Pi MCP (denna maskin)*, *Synka Pi MCP för öppna dokument*
* **Kraschen var dubbla plugin-kopior**: äldre byggen låg kvar i
  `~/.config/geany/plugins/` och `~/.local/share/geany/plugins/`, och Geany 2.1
  laddade dem *utöver* systemkopian → `plugin_init` kördes flera gånger
  (flera bryggor, flera flikar) och Geany kraschade. Nu installerar
  `make install` **endast** i `/usr/lib/x86_64-linux-gnu/geany/`, och pluginet
  har en init-vakt (`g_inited`) som skyddar mot dubbel initiering.
