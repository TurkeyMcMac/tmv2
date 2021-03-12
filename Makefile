VERSION = 0.1.7
CC = cc
CFLAGS = -Wall -Wextra -Wpedantic -std=c99 -D_POSIX_C_SOURCE=200112L\
 -DVERSION='"$(VERSION)"' $(OPTIMIZATION)
# Link to libcurses, which will link to the terminfo library and has a more
# consistent name than the terminfo library does:
LDLIBS = -lcurses
RM = rm -f

.PHONY: all
all: tmv2 tmv2.6.gz

tmv2: tmv2.c Makefile
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

tmv2.6.gz: tmv2.6
	gzip < $< > $@

tmv2.6: tmv2.6.in Makefile
	sed 's/@@VERSION@@/$(VERSION)/g' $< > $@

.PHONY: clean
clean:
	$(RM) tmv2 tmv2.6.gz tmv2.6
