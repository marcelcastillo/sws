# sws Makefile

CC = gcc
PROG = sws
OBJS = main.o cgi.o http.o server.o
CFLAGS = -Wall -Werror -Wextra -g

OS_NAME := $(shell uname -s)
ifeq ($(UNAME_S),SunOS)
CFLAGS  += -I/opt/magic/include
LDFLAGS += -L/opt/magic/lib
endif

LDFLAGS += -lmagic

all: ${PROG}

depend:
	mkdep -- ${CFLAGS} *.c

${PROG}: ${OBJS}
	@echo $@ depends on $?
	${CC} ${OBJS} -o ${PROG} ${LDFLAGS}

clean:
	rm -f ${PROG} ${OBJS}
