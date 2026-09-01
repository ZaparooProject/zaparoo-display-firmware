# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Zaparoo Project Contributors.

# Builds the firmware's canvas and scene code for the host and runs the layout
# regression tests. No ESP-IDF and no hardware: scenes are pure functions of the
# display state, so they compile and run anywhere.

MAIN := ../../main
# gnu17 rather than c17: the firmware uses strlcpy, which glibc only declares
# outside strict ANSI mode. ESP-IDF's libc provides it unconditionally.
CFLAGS := -std=gnu17 -O1 -g -Wall -Wextra -Werror -I$(MAIN)
SOURCES := render_test.c \
	$(MAIN)/ui/canvas.c \
	$(MAIN)/ui/scenes.c \
	$(MAIN)/ui/generated_fonts.c \
	$(MAIN)/ui/generated_logo.c

.PHONY: test clean
test: render_test
	./render_test

render_test: $(SOURCES)
	$(CC) $(CFLAGS) -o $@ $(SOURCES) -lm

clean:
	rm -f render_test
