# Makefile for 3DCad

PLATFORM ?= x64
ifeq ($(PLATFORM),x64)
LIBDIR = libs/glfw-3.4.bin.WIN64/lib-mingw-w64
else
LIBDIR = libs/glfw-3.4.bin.WIN32/lib-mingw-w64
endif

CC = gcc
CFLAGS = -std=c11 -O2 -Wall -Iinclude
LDFLAGS = -L$(LIBDIR)
# GLFW needs to be linked with GDI32 on MSYS2
# Passing --static so MSYS2 DLLs don't need to be included with the binary
LIBS = -lglfw3 -lopengl32 -lcomdlg32 -lgdi32 -lole32 -luuid -lshell32 --static

SRCS = $(wildcard src/*.c)
OBJDIR = build/$(PLATFORM)
OBJS = $(patsubst src/%.c,$(OBJDIR)/%.o,$(SRCS))
DEPS = $(OBJS:.o=.d)

TARGET = 3DCadGui.exe

.PHONY: all clean distclean run

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

$(OBJDIR)/%.o: src/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(OBJDIR):
	mkdir -p $@

-include $(DEPS)

clean:
	rm -f $(OBJS) $(DEPS) $(TARGET)

distclean: clean
	rm -rf build

run: all
	./$(TARGET)

