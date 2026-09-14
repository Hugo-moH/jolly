SOURCEDIR := $(abspath $(patsubst %/,%,$(dir $(abspath $(lastword \
	$(MAKEFILE_LIST))))))

BUILD_STATIC ?= 0
ENABLE_VULKAN ?= 0
USE_EXTERNAL_MD5 ?= 0

LIBS_REQUIRES :=

DOCS := ChangeLog LICENSE README

LINKER = $(CC)

LIBS = -lm

# Object dirs
MKDIRS := deps

include $(SOURCEDIR)/mk/common.mk

FLAGS := -std=c99 $(WARNINGS_DEF_C)

DEFINES :=

SHADERS_OUT := shaders
SHADERS_SRC := $(wildcard $(SOURCEDIR)/src/shaders/*.fs \
	$(SOURCEDIR)/src/shaders/*.vs)
SHADERS := $(subst $(SOURCEDIR)/src/,,$(SHADERS_SRC))
VK_SHADERS := $(wildcard $(SOURCEDIR)/src/shaders/*.vert \
	$(SOURCEDIR)/src/shaders/*.frag)
VK_SHADERS_BASE = $(basename $(addprefix $(SOURCEDIR)/src/,$@))
VK_SHADERS_TARGET := $(addsuffix .spv,$(subst $(SOURCEDIR)/src/,,$(VK_SHADERS)))

ifneq ($(BUILD_STATIC), 0)
	include $(BUILD_STATIC)/jg-static.mk
	BASE := $(NAME:%-jg=%)
	EXE := $(NAME)/$(NAME)$(BIN_EXT)
	CLEAN := $(NAME)
	TARGET := $(EXE)
	INSTALLDIR := $(NAME)
	SHARE_DEST := $(BASE)
	DEFINES += -DJGRF_STATIC
	LIBS += -L$(BUILD_STATIC) -l$(BASE)-jg $(LIBS_STATIC)
	ifneq ($(findstring $(CFLAGS_PTHREAD),$(LIBS_STATIC)),)
		PTHREAD := $(CFLAGS_PTHREAD)
	else
		PTHREAD :=
	endif
	ifneq ($(ASSETS),)
		ASSETS_PATH := $(ASSETS:%=$(BUILD_STATIC)/%)
		ASSETS_TARGET := $(ASSETS:%=$(NAME)/%)
		TARGET += $(ASSETS_TARGET)
	endif
	DESKTOP_TARGET := $(NAME)/$(NAME).desktop
	ifneq ($(ICONS),)
		ICONS_DIR := $(BUILD_STATIC)/icons
		ICONS_NAME := $(BASE)
		ICONS_PATH := $(ICONS:%=$(ICONS_DIR)/%)
	else
		ICONS_DIR := $(SOURCEDIR)/icons
		ICONS_NAME := jollygood
		ICONS_PATH := $(wildcard $(ICONS_DIR)/*.png) \
			$(ICONS_DIR)/$(ICONS_NAME).svg
		ICONS_BASE := $(notdir $(ICONS_PATH))
		ICONS := $(subst $(ICONS_NAME),$(BASE),$(ICONS_BASE))
	endif
	ICONS_SRC = $(subst icons/$(BASE),icons/$(ICONS_NAME),$@)
	ICONS_CPY = $(subst $(NAME)/icons,$(ICONS_DIR),$(ICONS_SRC))
	ICONS_TARGET := $(ICONS:%=$(NAME)/icons/%)
	SHADERS_BASE := $(notdir $(SHADERS))
	ifneq ($(ENABLE_VULKAN), 0)
		SHADERS_BASE += $(notdir $(VK_SHADERS_TARGET))
		SHADERS_SRC += $(VK_SHADERS_TARGET)
	endif
	SHADERS_TARGET := $(SHADERS_BASE:%=$(NAME)/shaders/%)
	TARGET += $(DESKTOP_TARGET) $(ICONS_TARGET) $(SHADERS_TARGET)
else
	NAME := jollygood
	BASE := $(NAME)
	EXE := $(NAME)$(BIN_EXT)
	CLEAN := $(EXE)
	TARGET := $(EXE)
	INSTALLDIR := $(SOURCEDIR)
	SHARE_DEST := jgrf
	PTHREAD :=
endif

CSRCS := jgrf.c \
	audio.c \
	cheats.c \
	cli.c \
	detect.c \
	dips.c \
	input.c \
	menu.c \
	osd.c \
	paths.c \
	settings.c \
	video.c \
	video_gl.c \
	deps/lodepng.c \
	deps/musl_getopt.c \
	deps/musl_memmem.c \
	deps/parson.c \
	deps/tconfig.c \
	deps/wave_writer.c

INCLUDES = -I$(DEPDIR) $(CFLAGS_JG) $(CFLAGS_EPOXY) $(CFLAGS_MINIZ) \
	$(CFLAGS_SDL3) $(CFLAGS_SPEEXDSP)
LIBS += $(LIBS_EPOXY) $(LIBS_MINIZ) $(LIBS_SDL3) $(LIBS_SPEEXDSP)

ICONS_INSTALL_DIR = $(DATAROOTDIR)/icons/hicolor/$${i}x$${i}/apps
SHADER_INSTALL_DIR := $(DATADIR)/jollygood/$(SHARE_DEST)/shaders

TARGET += $(SHADERS)

ifeq ($(USE_EXTERNAL_MD5), 0)
	CSRCS += deps/md5.c
else
	DEFINES += -DHAVE_OPENSSL
	INCLUDES += $(CFLAGS_LIBCRYPTO)
	LIBS += $(LIBS_LIBCRYPTO)
endif

ifneq ($(ENABLE_VULKAN), 0)
	CSRCS += video_vk.c
	DEFINES += -DJGRF_VULKAN
	INCLUDES += $(CFLAGS_VULKAN)
	LIBS += $(LIBS_VULKAN)
	TARGET += glslang
endif

ifneq ($(PLATFORM), Darwin)
	LIBS += -Wl,--no-undefined
endif

ifneq ($(PLATFORM), Windows)
	DEFINES += -DDATADIR="\"$(DATADIR)\"" -DLIBDIR="\"$(LIBDIR)\""
	FLAGS += -fvisibility=hidden
endif

# List of object files
OBJS := $(patsubst %,$(OBJDIR)/%,$(CSRCS:.c=.o)) $(OBJS_MINIZ)

# Compiler command
COMPILE_C = $(strip $(CC) $(CFLAGS) $(PTHREAD) $(CPPFLAGS) $(1) -c $< -o $@)

# Dependencies command
BUILD_DEPS = $(call COMPILE_C, $(FLAGS))

# Glslang command
BUILD_GLSLANG = $(GLSLANG) --quiet -V $(VK_SHADERS_BASE) -o $@

# Core command
BUILD_MAIN = $(call COMPILE_C, $(FLAGS) $(DEFINES) $(INCLUDES))

.PHONY: $(PHONY) glslang install-bin install-data install-man

all: $(TARGET)

$(OBJDIR)/%.o: $(SOURCEDIR)/src/%.c $(PREREQ)
	$(call COMPILE_INFO,$(BUILD_MAIN))
	@$(BUILD_MAIN)

$(OBJDIR)/deps/%.o: $(DEPDIR)/%.c $(PREREQ)
	$(call COMPILE_INFO,$(BUILD_DEPS))
	@$(BUILD_DEPS)

$(EXE): $(OBJS)
ifneq ($(BUILD_STATIC), 0)
	@mkdir -p $(NAME)
endif
	$(strip $(LINKER) -o $@ $^ $(LDFLAGS) $(LIBS))

$(SHADERS_OUT)/.tag:
	@mkdir -p -- $(SHADERS_OUT)
	@touch $@

$(SHADERS): $(SHADERS_SRC) $(SHADERS_OUT)/.tag
	@cp $(addprefix $(SOURCEDIR)/src/,$@) $@

glslang: $(VK_SHADERS_TARGET)

$(VK_SHADERS_TARGET): $(VK_SHADERS) $(SHADERS_OUT)/.tag
	$(call COMPILE_INFO,$(BUILD_GLSLANG))
	@$(BUILD_GLSLANG)

ifneq ($(BUILD_STATIC), 0)
ifneq ($(ASSETS),)
$(ASSETS_TARGET): $(ASSETS_PATH)
	@mkdir -p $(dir $@)
	@cp $(subst $(NAME),$(BUILD_STATIC),$@) $@
endif

$(DESKTOP_TARGET): $(BUILD_STATIC)/$(NAME).desktop
	@mkdir -p $(NAME)
	@cp $< $@

$(ICONS_TARGET): $(ICONS_PATH)
	@mkdir -p $(NAME)/icons
	@cp $(ICONS_CPY) $(NAME)/icons/$(notdir $@)

$(SHADERS_TARGET): $(SHADERS)
	@mkdir -p $(NAME)/shaders
	@cp $(subst $(NAME)/,,$@) $(NAME)/shaders
endif

clean:
	rm -rf $(OBJDIR) $(SHADERS_OUT) $(CLEAN)

install-bin: all
	@mkdir -p $(DESTDIR)$(BINDIR)
	cp $(EXE) $(DESTDIR)$(BINDIR)

install-data: all
	@mkdir -p $(DESTDIR)$(SHADER_INSTALL_DIR)
	@mkdir -p $(DESTDIR)$(DATAROOTDIR)/applications
	@mkdir -p $(DESTDIR)$(DATAROOTDIR)/icons/hicolor/scalable/apps
	@mkdir -p $(DESTDIR)$(DATAROOTDIR)/pixmaps
	cp $(INSTALLDIR)/$(NAME).desktop $(DESTDIR)$(DATAROOTDIR)/applications
	cp $(SHADERS_OUT)/default.vs $(DESTDIR)$(SHADER_INSTALL_DIR)
	cp $(SHADERS_OUT)/default.fs $(DESTDIR)$(SHADER_INSTALL_DIR)
	cp $(SHADERS_OUT)/aann.fs $(DESTDIR)$(SHADER_INSTALL_DIR)
	cp $(SHADERS_OUT)/crt-yee64.fs $(DESTDIR)$(SHADER_INSTALL_DIR)
	cp $(SHADERS_OUT)/crtea.fs $(DESTDIR)$(SHADER_INSTALL_DIR)
	cp $(SHADERS_OUT)/lcd.fs $(DESTDIR)$(SHADER_INSTALL_DIR)
	cp $(SHADERS_OUT)/sharp-bilinear.fs $(DESTDIR)$(SHADER_INSTALL_DIR)
ifneq ($(ENABLE_VULKAN), 0)
	for s in $(notdir $(VK_SHADERS_TARGET)); do \
		cp $(SHADERS_OUT)/$$s $(DESTDIR)$(SHADER_INSTALL_DIR); \
	done
endif
	for i in 32 48 64 96 128 256 512 1024; do \
		mkdir -p $(DESTDIR)$(ICONS_INSTALL_DIR); \
		if test "$$i" = '96' || test "$$i" = '1024'; then \
			cp $(INSTALLDIR)/icons/$(BASE)$$i.png \
				$(DESTDIR)$(DATADIR)/jollygood/$(SHARE_DEST)/; \
		fi; \
		cp $(INSTALLDIR)/icons/$(BASE)$$i.png \
			$(DESTDIR)$(ICONS_INSTALL_DIR)/$(BASE).png; \
	done
	cp $(INSTALLDIR)/icons/$(BASE).svg $(DESTDIR)$(DATAROOTDIR)/pixmaps/
	cp $(INSTALLDIR)/icons/$(BASE).svg \
		$(DESTDIR)$(DATAROOTDIR)/icons/hicolor/scalable/apps/
ifneq ($(BUILD_STATIC), 0)
ifneq ($(ASSETS),)
	@mkdir -p $(DESTDIR)$(DATADIR)/jollygood/$(SHARE_DEST)
	for a in $(ASSETS_TARGET); do \
		dir="$${a%/*}"; \
		if test "$$dir" != "$(NAME)"; then \
			dest="$(DATADIR)/jollygood/$(SHARE_DEST)/$${dir#*/}"; \
			mkdir -p $(DESTDIR)$$dest; \
			cp $$a $(DESTDIR)$$dest; \
		else \
			cp $$a $(DESTDIR)$(DATADIR)/jollygood/$(SHARE_DEST); \
		fi; \
	done
endif
endif

install-man: all
	@mkdir -p $(DESTDIR)$(MANDIR)/man6
	cp $(SOURCEDIR)/jollygood.6 $(DESTDIR)$(MANDIR)/man6

install: install-bin install-data install-docs install-man

install-strip: install
	$(STRIP) $(DESTDIR)$(BINDIR)/$(NAME)$(BIN_EXT)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(NAME)$(BIN_EXT)
	rm -rf $(DESTDIR)$(DOCDIR)
	rm -rf $(DESTDIR)$(DATADIR)/jollygood/$(SHARE_DEST)
	rm -f $(DESTDIR)$(DATAROOTDIR)/applications/$(NAME).desktop
	rm -f $(DESTDIR)$(DATAROOTDIR)/icons/hicolor/32x32/apps/$(BASE).png
	rm -f $(DESTDIR)$(DATAROOTDIR)/icons/hicolor/48x48/apps/$(BASE).png
	rm -f $(DESTDIR)$(DATAROOTDIR)/icons/hicolor/64x64/apps/$(BASE).png
	rm -f $(DESTDIR)$(DATAROOTDIR)/icons/hicolor/96x96/apps/$(BASE).png
	rm -f $(DESTDIR)$(DATAROOTDIR)/icons/hicolor/128x128/apps/$(BASE).png
	rm -f $(DESTDIR)$(DATAROOTDIR)/icons/hicolor/256x256/apps/$(BASE).png
	rm -f $(DESTDIR)$(DATAROOTDIR)/icons/hicolor/512x512/apps/$(BASE).png
	rm -f $(DESTDIR)$(DATAROOTDIR)/icons/hicolor/1024x1024/apps/$(BASE).png
	rm -f $(DESTDIR)$(DATAROOTDIR)/icons/hicolor/scalable/apps/$(BASE).svg
	rm -f $(DESTDIR)$(DATAROOTDIR)/pixmaps/$(BASE).svg
	rm -f $(DESTDIR)$(MANDIR)/man6/jollygood.6
