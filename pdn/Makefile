#	linmail-pdn Makefile
#
#	Builds the LinBPQ mail server as a standalone program that reaches a
#	packet.net node over RHPv2. Uses the upstream mail sources from the
#	directory above, unchanged, plus the shim in this directory. None of the
#	node code is linked.
#
#	make -C pdn            (or: cd pdn && make linmail-pdn)

TOP = ..
BUILD = build

# Upstream mail sources, as linked into LinBPQ
MAIL_SRCS = BBSUtilities.c BBSHTMLConfig.c FBBRoutines.c MailCommands.c \
 MailDataDefs.c MailRouting.c MailTCP.c MBLRoutines.c WPRoutines.c WebMail.c \
 NNTPRoutines.c lzhuf32.c

# Upstream support sources the mail code needs that carry no node state.
# HTMLCommonCode.c is GetTemplateFromFile, with the built-in page templates
# (templatedefs.c) LinBPQ uses.
SUPPORT_SRCS = Housekeeping.c UIRoutines.c utf8Routines.c md5.c compatbits.c \
 CMSAuth.c HTMLCommonCode.c

# The pdn shim
SHIM_SRCS = linmail-pdn.c pdnhost.c pdnweb.c pdnstubs.c

# Versions. LINBPQ_VERSION is G8BPQ's, from Versions.h (KVerstring). This
# build's own, PDN_VERSION, is <LinBPQ version>-pdn<n> for a release (set by
# the release workflow from the tag, e.g. 6.0.25.41-pdn1), <LinBPQ version>-dev
# otherwise. linmail-pdn --version prints both.
LINBPQ_VERSION := $(shell sed -n 's/^\#define KVerstring "\([0-9.]*\).*/\1/p' $(TOP)/Versions.h)
PDN_VERSION ?= $(LINBPQ_VERSION)-dev

CC = gcc
CFLAGS = -DLINBPQ -DPDN_LINMAIL -DNOMQTT -MMD -g -O1 -fcommon -fasynchronous-unwind-tables \
 -I$(TOP) -I. -I$(BUILD) -w $(EXTRA_CFLAGS)
SHIM_CFLAGS = -Wall -Wno-unused-variable -Wno-unused-function -Wno-pointer-sign \
 -Wno-unused-but-set-variable -Wno-format-truncation -Wno-stringop-truncation
LIBS = -ljansson -lconfig -lm -lz -lpthread

# For packages: jansson and libconfig linked in, so the binary needs only libc
# and runs across Debian releases (libconfig's soname differs between them)
STATIC_LIBS = -Wl,-Bstatic -ljansson -lconfig -Wl,-Bdynamic -lm -lz -lpthread

UPSTREAM_OBJS = $(addprefix $(BUILD)/,$(MAIL_SRCS:.c=.o) $(SUPPORT_SRCS:.c=.o))
SHIM_OBJS = $(addprefix $(BUILD)/,$(SHIM_SRCS:.c=.o))

OUT = linmail-pdn

all: $(OUT)

$(OUT): $(UPSTREAM_OBJS) $(SHIM_OBJS)
	$(CC) $(UPSTREAM_OBJS) $(SHIM_OBJS) $(LIBS) -o $@

# make -C pdn static: the packaged build
static: $(UPSTREAM_OBJS) $(SHIM_OBJS)
	$(CC) $(UPSTREAM_OBJS) $(SHIM_OBJS) $(STATIC_LIBS) -o linmail-pdn

# make -C pdn asan: linmail-pdn-asan, built with AddressSanitizer in its own
# build directory, for the tests (see .github/workflows/linmail-pdn.yml)
asan:
	$(MAKE) BUILD=build-asan OUT=linmail-pdn-asan \
		EXTRA_CFLAGS="-fsanitize=address -fno-omit-frame-pointer" \
		LIBS="-fsanitize=address $(LIBS)"

# make -C pdn deb ARCH=amd64 PDN_VERSION=6.0.25.41-pdn1: a .deb in pdn/dist
# (see packaging/build-deb.sh)
deb: static
	packaging/build-deb.sh $(ARCH) $(PDN_VERSION)

# make -C pdn -s linbpq-version: print G8BPQ's version (for the workflows)
linbpq-version:
	@echo $(LINBPQ_VERSION)

# make -C pdn -s upstream-sources: the upstream files linmail-pdn builds (for
# the upstream sync workflow's pull requests)
upstream-sources:
	@echo $(MAIL_SRCS) $(SUPPORT_SRCS)

# pdnversion.h is rewritten only when PDN_VERSION changes, so a new version
# rebuilds linmail-pdn.o and nothing else
$(BUILD)/pdnversion.h: FORCE | $(BUILD)
	@echo '#define PDN_VERSION "$(PDN_VERSION)"' > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp

$(BUILD)/linmail-pdn.o: $(BUILD)/pdnversion.h

$(BUILD)/%.o: $(TOP)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) $(SHIM_CFLAGS) -c $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

-include $(BUILD)/*.d

clean:
	rm -rf $(BUILD) build-asan linmail-pdn linmail-pdn-asan dist

.PHONY: all clean static deb asan linbpq-version upstream-sources FORCE
