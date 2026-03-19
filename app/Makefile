PROG    = antitailgate
SRCS    = main.c \
          config.c \
          token_manager.c \
          event_subscriber.c \
          event_publisher.c \
          input_trigger.c \
          alarm_handler.c \
          web_server.c \
          civetweb.c

PKGS    = glib-2.0

CFLAGS  += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --cflags $(PKGS))
CFLAGS  += -DNO_SSL=1 -DNO_CGI=1 -DNO_FILES=1 -DUSE_WEBSOCKET=0
CFLAGS  += -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
CFLAGS  += -D_GNU_SOURCE

LDFLAGS += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --libs $(PKGS))
LDFLAGS += -laxevent -laxparameter -lpthread -lm -lcurl

OBJS    = $(SRCS:.c=.o)

.PHONY: all clean

all: $(PROG)

$(PROG): $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	$(RM) $(PROG) $(OBJS)
