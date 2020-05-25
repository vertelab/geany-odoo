
all : templates.tmp tags.tmp geany.tmp 
	@echo Complete

templates.tmp:
	@cp templates/files/* ~/.config/geany/templates/files
	@touch templates.tmp

tags.tmp: 
	@cp tags/odoo.py.tags ~/.config/geany/tags
	@touch tags.tmp

geany.tmp: snippets.conf
	@cp snippets.conf ~/.config/geany/snippets.conf
	@touch snippets.tmp


clean:
	@rm -f *pyc
	@rm -f *tmp
	@echo "Cleaned up"


