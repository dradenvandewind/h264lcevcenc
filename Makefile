# Makefile pour le plugin GStreamer Dual Encoder
CC = gcc
CFLAGS = -Wall -Wextra -fPIC -O2
PKG_CONFIG = pkg-config

# Packages requis
GSTREAMER_PACKAGES = gstreamer-1.0 gstreamer-video-1.0 gstreamer-base-1.0

# Flags de compilation
GSTREAMER_CFLAGS = $(shell $(PKG_CONFIG) --cflags $(GSTREAMER_PACKAGES))
GSTREAMER_LIBS = $(shell $(PKG_CONFIG) --libs $(GSTREAMER_PACKAGES))

# Bibliothèques x264 et xeve
X264_CFLAGS = -I/usr/local/include
X264_LIBS = -lx264

INCLUDE_PATHS = /usr/include/xeve /usr/local/include/xeve /usr/local/include/
XEVE_CFLAGS = $(foreach dir,$(INCLUDE_PATHS),-I$(dir))



#XEVE_CFLAGS = -I/usr/include/xeve
XEVE_LIBS = -lxeve

# Combiner tous les flags
ALL_CFLAGS = $(CFLAGS) $(GSTREAMER_CFLAGS) $(X264_CFLAGS) $(XEVE_CFLAGS)
ALL_LIBS = $(GSTREAMER_LIBS) $(X264_LIBS) $(XEVE_LIBS)

# Nom du plugin
PLUGIN_NAME = libgstdualencoder.so

# Répertoire d'installation
PLUGIN_DIR = $(shell $(PKG_CONFIG) --variable=pluginsdir gstreamer-1.0)

# Sources - CORRIGÉ: le fichier s'appelle gstdualencoder.c, pas gst-dual-encoder.c
SOURCES = gstdualencoder.c

.PHONY: all clean install

all: $(PLUGIN_NAME)

$(PLUGIN_NAME): $(SOURCES)
	$(CC) $(ALL_CFLAGS) -shared -o $@ $< $(ALL_LIBS)

clean:
	rm -f $(PLUGIN_NAME)

install: $(PLUGIN_NAME)
	cp $(PLUGIN_NAME) $(PLUGIN_DIR)/

# Test si les dépendances sont installées
check-deps:
	@echo "Vérification des dépendances..."
	@$(PKG_CONFIG) --exists $(GSTREAMER_PACKAGES) || (echo "Erreur: GStreamer non trouvé" && exit 1)
	@echo "GStreamer: OK"
	@test -f /usr/include/x264.h || (echo "Erreur: x264 headers non trouvés" && exit 1)
	@echo "x264: OK"
	@test -f /usr/include/xeve/xeve.h || (echo "Erreur: xeve headers non trouvés" && exit 1)
	@echo "xeve: OK"
	@echo "Toutes les dépendances sont installées!"