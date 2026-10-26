BIN     := $(HOME)/.local/bin/yabai-swipe
LABEL   := com.pxdl.yabai-swipe
PLIST   := $(HOME)/Library/LaunchAgents/$(LABEL).plist
DOMAIN  := gui/$(shell id -u)

build/yabai-swipe: yabai-swipe.c
	mkdir -p build
	clang -O2 -Wall -Wextra -o $@ $< -framework ApplicationServices
	codesign -fs - -i $(LABEL) $@

install: build/yabai-swipe
	install -m 755 build/yabai-swipe $(BIN)
	sed "s|@BIN@|$(BIN)|;s|@HOME@|$(HOME)|" $(LABEL).plist > $(PLIST)
	-launchctl bootout $(DOMAIN)/$(LABEL) 2>/dev/null
	launchctl bootstrap $(DOMAIN) $(PLIST)

restart:
	launchctl kickstart -k $(DOMAIN)/$(LABEL)

uninstall:
	-launchctl bootout $(DOMAIN)/$(LABEL)
	rm -f $(PLIST) $(BIN)

clean:
	rm -rf build

.PHONY: install restart uninstall clean
