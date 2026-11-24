# Makefile - estacion (C11 + libcurl + SQLite amalgamation)
# Las recetas empiezan con '>' en vez de TAB (más fácil de copiar y pegar).
.RECIPEPREFIX = >

CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -O2 -Ilib/sqlite
LDLIBS  = -lcurl -lpthread -ldl -lm

SRC     = $(wildcard src/*.c)
OBJ     = $(patsubst src/%.c,build/%.o,$(SRC))
SQLITE  = build/sqlite3.o

all: estacion

estacion: $(OBJ) $(SQLITE)
> $(CC) -o $@ $^ $(LDLIBS)

build/%.o: src/%.c $(wildcard src/*.h) | build
> $(CC) $(CFLAGS) -c $< -o $@

# SQLite se compila aparte, una sola vez (tarda unos segundos)
$(SQLITE): lib/sqlite/sqlite3.c | build
> $(CC) -O2 -c $< -o $@

build:
> mkdir -p build

run: estacion
> ./estacion config

clean:
> rm -rf build estacion

.PHONY: all run clean
