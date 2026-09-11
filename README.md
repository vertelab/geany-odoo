This repository is a collection of configuration files to help developers of **Odoo** modules that use the editor **Geany** for their work.
To install the files is possible to follow the instruction:

## installation
```
wget -O- https://raw.githubusercontent.com/vertelab/geany-odoo/master/install | bash
```
## PrettyPrinter
1. Install as below.<br>
2. Open "Tools >> Extentions"<br>
3. Find, mark and select XML PrettyPrinter.
```
sudo apt install geany-plugin-prettyprinter
```

## templates

Contains the filetype to create rapidly new standard openerp file

*USE*

Go to File -> New from template

and select your base file from:

* \_\_openerp\_\_.py   [7.0-9.0]
* \_\_manifest\_\_.py   [10.0-]
* openerp_class.py     [7.0-9.0]
* openerp_view.xml     [7.0-9.0]
* odoo_class.py        [10.0-]
* odoo_views.xml       [10.0-]
* wizard.py
* wizard_view.xml

## tags

Contains the Odoo ORM functions declaration to show the correct use of a function while write it

*USE*

Write an Odoo Model function to see the correct declaration in a tooltip

## snippets.conf

Contains the definition of different snippets of code used within the files .py

*USE*

Just type keywords below and press TAB button to get the auto-insertion of the snippet

**PYTHON CODE**

* class = class structure
* cols = _column structure
* defs = _defaults structure
* char = field char
* integ = field integer
* bool = field boolean
* float = field float
* text = field text
* date = field date
* datetime = field datetime
* selec = field selection
* o2m = field one2many
* m2o = field many2one
* m2m = field many2many
* related = field related
* fnct = field function
* def_fnct = function definition related to function field
* super = super for function inherit
* raise = raise error message
* raise7 = raise error message for OpenERP 7
* pdb = debugger import
* python\_py = \_\_openerp\_\_.py file structure
* create = create ORM method
* write = write ORM method
* unlink = unlink ORM method
* browse = browse ORM method
* search = search ORM method
* copy = copy ORM method
* copy_data = copy_data ORM method

**XML CODE**

* field = field tag
* tree = new tree structure
* tree_in = inherited tree structure
* form = new form structure
* form7 = new form structure with OpenERP 7 specific tag
* form_in = inherited form structure
* xpath = xpath structure
* kanban = kanban structure
* menu = menu structure
* search = search structure
* search_in = inherited search structure
* action = action structure
* button = object button
* button_action = action button
* notebook = notebook structure
* filter = filter field in search view
* group_by = filter field with group by context
* context = context field
* domain = domain field
* help = help tag used in the action structure
* attrs = complete attrs field tag

## geany-pi — Geany plugin

Native Geany plugin source lives in [`geany-pi/`](geany-pi/):

* **Multiterm** tab — one `$SHELL` terminal per document directory; documents
  opened over gvfs/sftp get `ssh -t <host> "cd <dir> && exec $SHELL -l"`.
* **Pi** tab — a separate Pi coding-agent session.
* **Agent surface** — a UNIX-socket bridge for external agents
  (`geany-pi-ctl`), `geany-pi-git` (git at the document's machine) and
  `geany-pi-mcp-router` (Odoo `ai_pi_mcp` MCP, routed per active document).

Configuration is done in Geany's Plugin Manager → Preferences (no Tools menu
items).

## geany-pfiles — p-file support

Source in [`geany-pfiles/`](geany-pfiles/). Odoo modules here are written
version-agnostically as *p-files* (`x.p.py`, `x.p.xml`, …) and expanded per
edition with `preprocess -f -D VERSION=<v> -o <out> <in>`.

* editions are a comma separated list in the plugin's own preferences
  (e.g. `16.0,17.0,18.0`);
* one entry per edition — a **P-files** toolbar drop-down without limit, plus
  one item per edition in Geany's **Build** menu (Geany's non-filetype group is
  fixed at four slots and `build_set_group_count()` is private, so more
  editions than that live in the toolbar menu);
* the expansion runs **on the machine that owns the document** — resolved from
  the gvfs/sftp path itself (ssh) or locally, the same target resolution as the
  Multiterm tab;
* a *git configuration* action deploys what p-files need in the document's
  repository on that machine: a managed `.gitattributes` block, a `pre-commit`
  hook expanding staged p-files for the version named by the current branch
  (branch-per-version), and `core.hooksPath` pointing at it.

Directive syntax (verified against PyPI `preprocess` 2.0.0 and the production
p-files in `/usr/share/odoo-*`): `# #if VERSION >= "18.0"` … `# #endif` in
`.p.py` (**double hash** — the comment prefix plus the `#if` statement) and
`<!-- #if … -->` in `.p.xml`. `-f` is required for re-runs.

## geany-mermaid — Mermaid diagram preview

Source in [`geany-mermaid/`](geany-mermaid/). Renders the ```` ```mermaid ````
blocks (and `@startmermaid` … `@endmermaid`) of the active document with
**mmdc** and shows them in a plugin window — toolbar button or
`Ctrl+Shift+M`. PNG + `GdkPixbuf` rather than SVG, because Geany does not
depend on librsvg. Rendering is asynchronous and queued; `mmdc` comes from the
`workstation.md2pdf` Salt state.

### Install (Salt)

Salt pulls **the current code from this repository** on every run and rebuilds
every plugin that changed — there is no copy of the plugin source on the Salt
master:

```
salt <minion> state.apply workstation.geany pillar='{"user": "waland"}'
```

Manual build: `cd geany-pi && make && make install` (likewise for
`geany-pfiles` and `geany-mermaid`).

Requires Ubuntu 24.04, Geany 2.1 (`ppa:ubuntuhandbook1/geany`),
`libvte-2.91-dev` (geany-pi) and `preprocess` (geany-pfiles, installed by the
Salt state; `mmdc` for geany-mermaid comes from `workstation.md2pdf`).
