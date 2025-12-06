CC = gcc
PROG = sws
OBJS = main.o cgi.o http.o server.o

CFLAGS  = -Wall -Werror -Wextra -g
LDFLAGS = -lmagic

OMNIOS_CFLAGS  = -I/opt/magic/include
OMNIOS_LDFLAGS = -L/opt/magic/lib -R/opt/magic/lib -libsocket -lnsl

all: $(PROG)

%.o: %.c
	@echo Compiling $< to $@
	@if uname -s | grep -q SunOS; then \
		EXTRA_CFLAGS="$(OMNIOS_CFLAGS)"; \
	else \
		EXTRA_CFLAGS=""; \
	fi; \
	$(CC) $(CFLAGS) $$EXTRA_CFLAGS -c $< -o $@

$(PROG): $(OBJS)
	@echo Building $@ from $?
	@if uname -s | grep -q SunOS; then \
		EXTRA_LDFLAGS="$(OMNIOS_LDFLAGS)"; \
	else \
		EXTRA_LDFLAGS=""; \
	fi; \
	$(CC) $(CFLAGS) $(OBJS) -o $(PROG) $(LDFLAGS) $$EXTRA_LDFLAGS

clean:
	rm -f $(PROG) $(OBJS)
