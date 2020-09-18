NULL=

AM_CPPFLAGS = \
	-DPACKAGE_DATA_DIR=\""$(datadir)/stuurman-desktop"\" \
	-DPACKAGE_UI_DIR=\""$(datadir)/stuurman-desktop/ui"\" \
	-DPACKAGE_LOCALE_DIR=\""$(prefix)/$(DATADIRNAME)/locale"\"

bin_PROGRAMS = stuurman-desktop

stuurman_desktop_SOURCES = \
	pcmanfm.c \
	pcmanfm.h \
	app-config.c \
	app-config.h \
	desktop.c \
	desktop.h \
	desktop-manager.c \
	desktop-manager.h \
	window-tracker.c \
	window-tracker.h \
	wallpaper-manager.c \
	wallpaper-manager.h \
	pref.c \
	pref.h \
	utils.c \
	utils.h \
	gseal-gtk-compat.h \
	single-inst.c \
	single-inst.h \
	$(NULL)

EXTRA_DIST= \
	cell-placement-generator.h \
	main-win-ui.c \
	desktop-ui.c \
	$(NULL)

stuurman_desktop_CFLAGS = \
	$(XLIB_CFLAGS) \
	$(FM_CFLAGS) \
	$(G_CAST_CHECKS) \
	-Wall \
	-Werror-implicit-function-declaration \
	$(NULL)

stuurman_desktop_LDADD = \
	$(XLIB_LIBS) \
	$(FM_LIBS) \
	$(NULL)

noinst_PROGRAMS=xml-purge
xml_purge_SOURCES=xml-purge.c
xml_purge_CFLAGS=$(GIO_CFLAGS)
xml_purge_LDADD=$(GIO_LIBS)
