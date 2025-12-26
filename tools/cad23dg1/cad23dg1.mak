# Makefile to build my little command line frontend for the components I've cherrypicked
# replaces gcc -Iinclude src/cad_file.c src/cad_export_3dg1.c cad23dg1.c -o cad23dg1.exe

CC := gcc
CFLAGS := -O2 -Wall
INCLUDES := -Iinclude
SRCS := src/cad_file.c src/cad_export_3dg1.c cad23dg1.c
TARGET := cad23dg1.exe

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(INCLUDES) $(SRCS) -o $(TARGET)

clean:
	-@rm -f $(TARGET)