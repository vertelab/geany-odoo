# geany-pfiles — p-file support for Geany

A Geany plugin for version-agnostic Odoo development with **p-files**:
`x.p.py`, `x.p.xml`, … contain `#if VERSION >= "18.0"` blocks and are expanded
per edition with

```sh
preprocess -D VERSION=18.0 -o x.py x.p.py
```

The plugin covers four things:

1. **Editions as configuration** — a comma separated list in the plugin's own
   preferences (e.g. `16.0,17.0,18.0`), editable in
   *Edit → Plugin Manager → geany-pfiles → Preferences*.
2. **One build entry per edition** — a toolbar drop-down **P-files** with one
   item per edition, plus one item per edition in Geany's **Build** menu.
3. **Runs on the machine that owns the document** — a file opened over
   gvfs/sftp (`/run/user/…/gvfs/sftp:host=192.168.11.145/…`) is expanded on
   that host over `ssh`, a local file locally. Same target resolution as the
   Multiterm tab in `geany-pi`.
4. **Deploys the git configuration p-files need** — a managed block in
   `.gitattributes`, a `pre-commit` hook that expands staged p-files for the
   version named by the current branch, and `core.hooksPath` pointing at it
   (all in the document's repository, on the document's machine).

## Requirements

| | |
|---|---|
| Geany | 2.x (`geany` + `geany-dev`, GTK3) |
| preprocess | `pip install preprocess` (the `preprocess` command) |
| ssh | only for documents opened over gvfs/sftp |

## Build & install

```sh
make                 # -> geany-pfiles.so
sudo make install    # -> /usr/lib/x86_64-linux-gnu/geany/ + ~/.local/bin/geany-pfiles
```

On the workstations this is deployed by Salt (`workstation/geany`), which
fetches `vertelab/geany-odoo` and builds every plugin in it.

**Never keep a second copy** of the same plugin in a user plugin directory:
Geany 2.1 scans both `~/.config/geany/plugins` and
`~/.local/share/geany/plugins`, and a duplicate `plugin_init` crashes Geany.

## Usage

Open a p-file and pick an edition from the **P-files** toolbar button, or use
the per-edition entries in the **Build** menu. Output goes to Geany's
*Messages* tab and to the plugin log
(`~/.config/geany/geany-pfiles/log.txt`).

The expanded file (`models/x.p.py` → `models/x.py`) lands next to the source,
on the same machine.

### Git setup for p-files

*Preferences → "Skicka ut git-konfiguration"* (or the toolbar item
**Git-konfiguration för p-filer…**) run:

```
geany-pfiles git-config <active file>
```

which, in the repository containing the file, on the file's machine:

* keeps a managed block in **`.gitattributes`** (between
  `# >>> geany-pfiles >>>` and `# <<< geany-pfiles <<<`):

  ```gitattributes
  *.p.py  linguist-language=Python     text eol=lf
  *.p.js  linguist-language=JavaScript text eol=lf
  *.p.xml linguist-language=XML        text eol=lf
  *.p.csv linguist-language=CSV        text eol=lf
  ```

* writes **`<hooksPath>/pre-commit`** (default `.githooks`), which expands the
  staged p-files for the version named by the current branch — the
  branch-per-version model: on branch `18.0` the hook expands with
  `VERSION=18.0` and stages the result, so a commit can never contain a stale
  expansion. On a branch that matches no edition it skips silently. Override
  with `GEANY_PFILES_VERSION`, change the command with
  `GEANY_PFILES_PREPROCESS`.
* sets **`git config core.hooksPath <hooksPath>`** so the hook is
  version-controlled and shared by the whole team.

The hook is idempotent and the `.gitattributes` block is rewritten in place,
so it is safe to re-run after changing the editions list.

## Settings

`~/.config/geany/geany-pfiles/geany-pfiles.conf` (0600):

```ini
[editions]
list=17.0,18.0,19.0
preprocess_cmd=preprocess
defines=-D REPO=my_module

[git]
hooks_path=.githooks
precommit=true
```

The same values are edited in the plugin's Preferences dialog, which also has
*Uppdatera Bygg-menyn nu* and *Skicka ut git-konfiguration* buttons.

## The Build menu: a hard limit of four

Geany's build-menu groups have a **fixed** number of slots (`build_set_group_count()`
and `build_menu_update()` are private to Geany, so a plugin cannot grow them):
non-filetype = 4, filetype = 3, execute = 2.

The plugin therefore

* writes the non-filetype slots (`NF_xx_LB/_CM/_WD`) into `geany.conf` at
  plugin init, replacing any legacy `Preprocess <v>` entries it finds and
  leaving all other build entries alone, and
* patches the live build items with `build_set_menu_item()` and removes
  surplus slots with `build_remove_menu_item()`.

If more editions are configured than there are slots, the status bar says so
and the **toolbar drop-down**, which has no such limit, covers the rest.
Structural changes (a longer edition list) need a Geany restart; the labels
and commands of existing slots are updated immediately.

## Layout

```
geany-pfiles/
├── Makefile
├── README.md
├── scripts/geany-pfiles     # helper: preprocess / git-config / where
└── src/
    ├── geany-pfiles.c       # plugin: prefs, toolbar, build menu, spawn
    └── geany-pfiles.h
```

The helper is deliberately a separate script: it is the part that knows about
gvfs paths and ssh, and it can be run by hand:

```sh
geany-pfiles where        /run/user/1000/gvfs/sftp:host=host/usr/share/x/a.p.py
geany-pfiles preprocess   18.0 a.p.py
geany-pfiles git-config   a.p.py --editions 16.0,17.0,18.0
```

## Notes

* Processes are spawned with `g_spawn_async_with_pipes()` + `GIOChannel` +
  `g_child_watch_add()`; `g_spawn_sync()`/`popen()`/`waitpid()` deadlock inside
  Geany's main loop and are never used.
* The plugin makes itself resident and unregisters its toolbar item in
  `plugin_cleanup()` so it can be disabled/enabled in the Plugin Manager.
