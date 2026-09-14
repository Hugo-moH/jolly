DIR_MINIZ := $(wildcard $(DEPDIR)/miniz)

ifneq ($(DIR_MINIZ),)
	USE_EXTERNAL_MINIZ ?= 0
else
	override USE_EXTERNAL_MINIZ := 1
endif

ifeq ($(USE_EXTERNAL_MINIZ), 0)
	CFLAGS_MINIZ := -I$(DIR_MINIZ)
	LIBS_MINIZ :=
	MKDIRS += deps/miniz
	SRCS_MINIZ := deps/miniz/miniz.c
	OBJS_MINIZ := $(patsubst %,$(OBJDIR)/%,$(SRCS_MINIZ:.c=.o))
else
	CFLAGS_MINIZ = $(shell $(PKG_CONFIG) --cflags miniz)
	LIBS_MINIZ = $(shell $(PKG_CONFIG) --libs miniz)
	OBJS_MINIZ :=
endif

ifneq (,$(and $(findstring miniz,$(LIBS_REQUIRES)), \
		$(filter-out 0,$(USE_EXTERNAL_MINIZ))))
	override REQUIRES_PRIVATE += miniz
endif

ifneq ($(DIR_MINIZ),)
FLAGS_MINIZ := -std=c99 $(WARNINGS_DEF_C)

ifneq ($(PLATFORM), Windows)
	FLAGS_MINIZ += -fvisibility=hidden
endif

BUILD_MINIZ = $(call COMPILE_C, $(FLAGS_MINIZ))

$(OBJDIR)/deps/miniz/%.o: $(DIR_MINIZ)/%.c $(OBJDIR)/.tag
	$(call COMPILE_INFO,$(BUILD_MINIZ))
	@$(BUILD_MINIZ)

ifeq ($(USE_EXTERNAL_MINIZ), 0)
install-docs::
	cp $(DIR_MINIZ)/LICENSE $(DESTDIR)$(DOCDIR)/LICENSE-miniz
endif
endif
