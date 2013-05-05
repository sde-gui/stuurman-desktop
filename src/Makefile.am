NULL=

AM_CPPFLAGS = \
	-DPACKAGE_DATA_DIR=\""$(datadir)/pcmanfm"\" \
	-DPACKAGE_UI_DIR=\""$(datadir)/pcmanfm/ui"\" \
	-DPACKAGE_LOCALE_DIR=\""$(prefix)/$(DATADIRNAME)/locale"\"

bin_PROGRAMS = pcmanfm-desktop

pcmanfm_desktop_SOURCES = \
	pcmanfm.c \
	pcmanfm.h \
	app-config.c \
	app-config.h \
	desktop.c \
	desktop.h \
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
	main-win-ui.c \
	desktop-ui.c \
	$(NULL)

pcmanfm_desktop_CFLAGS = \
	$(XLIB_CFLAGS) \
	$(FM_CFLAGS) \
	$(G_CAST_CHECKS) \
	-Wall \
	-Werror-implicit-function-declaration \
	$(NULL)

pcmanfm_desktop_LDADD = \
	$(XLIB_LIBS) \
	$(FM_LIBS) \
	$(NULL)

noinst_PROGRAMS=xml-purge
xml_purge_SOURCES=xml-purge.c
xml_purge_CFLAGS=$(GIO_CFLAGS)
xml_purge_LDADD=$(GIO_LIBS)
