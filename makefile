CC = gcc
CFLAGS = -Wall -Wextra -Werror -g
SRCS = $(wildcard *.c)
OBJS = $(SRCS:.c=.o)
EXEC = sws

.PHONY: all clean

all: $(EXEC)

$(EXEC): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(EXEC)
