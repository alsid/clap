GL_INCLUDE = /usr/X11R6/include
GL_LIB = /usr/X11R6/lib

CC := gcc
LD := $(CC)
PLAT_OBJS := display-glfw.o pngloader.o
CFLAGS := $(CFLAGS) -I$(GL_INCLUDE) $(shell pkg-config --cflags freetype2)
LDFLAGS := -L$(GL_LIB) -lGL -lGLEW -lglfw -lpng $(shell pkg-config --libs freetype2)
ifneq ($(DEBUG),)
CFLAGS += -fsanitize=address
LDFLAGS := -lasan $(LDFLAGS)
endif
# -lfreetype
