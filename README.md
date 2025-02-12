This repository is a collection of configuration files to help developers of **Odoo** modules that use the editor **Geany** for their work.
To install the files is possible to follow the instruction:

## installation
```
wget -O- https://raw.githubusercontent.com/vertelab/geany-odoo/master/install | bash
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
