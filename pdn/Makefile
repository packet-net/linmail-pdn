#	linmail-pdn Makefile
#
#	Builds the LinBPQ mail server as a standalone program that reaches a
#	packet.net node over RHPv2. Uses the upstream mail sources from the
#	directory above, unchanged apart from a few PDN_LINMAIL blocks, plus the
#	shim in this directory. None of the node code is linked.
#
#	make -C pdn            (or: cd pdn && make linmail-pdn)

TOP = ..
BUILD = build

# Upstream mail sources, as linked into LinBPQ
MAIL_SRCS = BBSUtilities.c BBSHTMLConfig.c FBBRoutines.c MailCommands.c \
 MailDataDefs.c MailRouting.c MailTCP.c MBLRoutines.c WPRoutines.c WebMail.c \
 NNTPRoutines.c lzhuf32.c

# Upstream support sources the mail code needs that carry no node state
SUPPORT_SRCS = Housekeeping.c UIRoutines.c utf8Routines.c md5.c compatbits.c \
 CMSAuth.c

# The pdn shim
SHIM_SRCS = linmail-pdn.c pdnhost.c pdnstubs.c

CC = gcc
CFLAGS = -DLINBPQ -DPDN_LINMAIL -DNOMQTT -MMD -g -O1 -fcommon -fasynchronous-unwind-tables \
 -I$(TOP) -I. -w $(EXTRA_CFLAGS)
SHIM_CFLAGS = -Wall -Wno-unused-variable -Wno-unused-function -Wno-pointer-sign \
 -Wno-unused-but-set-variable -Wno-format-truncation -Wno-stringop-truncation
LIBS = -ljansson -lconfig -lm -lz -lpthread

UPSTREAM_OBJS = $(addprefix $(BUILD)/,$(MAIL_SRCS:.c=.o) $(SUPPORT_SRCS:.c=.o))
SHIM_OBJS = $(addprefix $(BUILD)/,$(SHIM_SRCS:.c=.o))

all: linmail-pdn

linmail-pdn: $(UPSTREAM_OBJS) $(SHIM_OBJS)
	$(CC) $(UPSTREAM_OBJS) $(SHIM_OBJS) $(LIBS) -o $@

$(BUILD)/%.o: $(TOP)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) $(SHIM_CFLAGS) -c $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

-include $(BUILD)/*.d

clean:
	rm -rf $(BUILD) linmail-pdn

.PHONY: all clean
