BIN    := $(HOME)/.local/bin/yabai-swipe
LABEL  := com.pxdl.yabai-swipe
PLIST  := $(HOME)/Library/LaunchAgents/$(LABEL).plist
DOMAIN := gui/$(shell id -u)
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic
# Code-signing identity. Ad-hoc ("-") changes with every build, so macOS drops the
# Accessibility grant; a self-signed certificate keeps it (see README).
SIGN   ?= -

build/yabai-swipe: yabai-swipe.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $< -framework ApplicationServices
	codesign --force --sign "$(SIGN)" --identifier $(LABEL) $@

install: build/yabai-swipe
	@mkdir -p $(dir $(BIN)) $(dir $(PLIST))
	install -m 755 build/yabai-swipe $(BIN)
	sed -e "s|@BIN@|$(BIN)|" -e "s|@HOME@|$(HOME)|" $(LABEL).plist > $(PLIST)
	@# bootout returns before the job is gone; bootstrapping too early fails with error 5.
	@launchctl bootout $(DOMAIN)/$(LABEL) 2>/dev/null; \
	for i in $$(seq 50); do launchctl print $(DOMAIN)/$(LABEL) >/dev/null 2>&1 || break; sleep 0.1; done
	launchctl bootstrap $(DOMAIN) $(PLIST)

restart:
	launchctl kickstart -k $(DOMAIN)/$(LABEL)

# Cross-checks the Space index computation against yabai's view of the display under the cursor.
check: build/yabai-swipe
	@for step in 1 -1; do \
	  dir=$$([ $$step = 1 ] && echo next || echo prev); \
	  want=$$(yabai -m query --spaces --display mouse | jq --argjson s $$step \
	    '((map(."is-visible") | index(true)) + $$s) as $$t | if $$t < 0 or $$t >= length then -1 else .[$$t].index end'); \
	  got=$$(build/yabai-swipe --target $$dir); \
	  echo "$$dir: yabai=$$want yabai-swipe=$$got"; \
	  [ "$$got" = "$$want" ] || exit 1; \
	done

uninstall:
	-launchctl bootout $(DOMAIN)/$(LABEL)
	rm -f $(PLIST) $(BIN)

clean:
	rm -rf build

.PHONY: install restart check uninstall clean
