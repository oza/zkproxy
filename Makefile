CFLAGS += -g -O3 -Wall -Wstrict-prototypes -I./include
CFLAGS += -D_GNU_SOURCE -DNDEBUG
LIBS += -lpthread -lacrd

PROGRAMS = zkproxy
ZKPROXY_OBJS = zkproxy.o coroutine.o event.o net.o logger.o zkproxynet.o recordio.o zookeeper.jute.o
ZKPROXY_DEP = $(ZKPROXY_OBJS:.o=.d)

.PHONY:all
all: $(PROGRAMS)

zkproxy: $(ZKPROXY_OBJS)
	$(CC) $^ -o $@ $(LIBS)

-include $(ZKPROXY_DEP)

%.o: %.c
	$(CC) -c $(CFLAGS) $*.c -o $*.o
	@$(CC) -MM $(CFLAGS) -MF $*.d -MT $*.o $*.c

.PHONY:clean
clean:
	rm -f *.[od] $(PROGRAMS)

# support for GNU Flymake
check-syntax:
	$(CC) $(CFLAGS) -fsyntax-only $(CHK_SOURCES)
