CC = gcc
CFLAGS = -O2 -Wall -nodefaultlibs
LDFLAGS = -lc -lm -lpthread -lz
SRC = src/hyperpack.c
BIN = hyperpack

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(BIN) *.hpk

.PHONY: all clean

# ===== WASM Build (requires Emscripten SDK) =====
.PHONY: wasm wasm-clean

wasm:
	bash build-wasm.sh

wasm-clean:
	rm -f hyperpack-web/public/hyperpack.js hyperpack-web/public/hyperpack.wasm
