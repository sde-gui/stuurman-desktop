NULL=

# GtkBuilder UI definition files
uidir=$(datadir)/stuurman-desktop/ui
ui_in_files= \
	desktop-pref.glade \
	$(NULL)
ui_DATA = $(ui_in_files:.glade=.ui)

EXTRA_DIST = \
	$(ui_DATA) \
	$(NULL)

# Purge GtkBuilder UI files
.glade.ui:
	test -f $(top_builddir)/src/xml-purge || \
		(cd $(top_builddir)/src && $(MAKE) xml-purge)
	cp $< $@
	$(top_builddir)/src/xml-purge $@
