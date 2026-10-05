# Convenience wrapper around the qmake build.
#
# LibreCAD 2.2.1.x links Qt 5.15, so the plugin must be built with the Qt 5
# qmake. On Arch that is /usr/bin/qmake; qmake6 produces a plugin that will
# not load.
QMAKE       ?= /usr/bin/qmake
BUILD_DIR   ?= build-tmp
PLUGIN_DIR  ?= $(HOME)/.librecad/plugins
PLUGIN       = build/liblc_pybridge.so

.PHONY: all clean install uninstall loadtest dispatchtest check test

all: $(PLUGIN)

$(PLUGIN): plugin/lc_pybridge.cpp plugin/lc_pybridge.h plugin/lc_pybridge.pro
	@mkdir -p "$(BUILD_DIR)"
	cd "$(BUILD_DIR)" && $(QMAKE) ../plugin/lc_pybridge.pro
	$(MAKE) -C "$(BUILD_DIR)"

loadtest:
	@mkdir -p "$(BUILD_DIR)/loadtest"
	cd "$(BUILD_DIR)/loadtest" && $(QMAKE) ../../tools/loadtest/loadtest.pro
	$(MAKE) -C "$(BUILD_DIR)/loadtest"

dispatchtest:
	@mkdir -p "$(BUILD_DIR)/dispatchtest"
	cd "$(BUILD_DIR)/dispatchtest" && $(QMAKE) ../../tools/dispatchtest/dispatchtest.pro
	$(MAKE) -C "$(BUILD_DIR)/dispatchtest"

# Run the dispatch layer against a stub document. No LibreCAD process needed.
test: dispatchtest
	"$(BUILD_DIR)/dispatchtest/dispatchtest"

# Load the plugin exactly the way LibreCAD does and print its metadata.
# QPluginLoader needs an absolute path; the quoting survives paths with spaces.
check: all loadtest test
	QT_QPA_PLATFORM=offscreen "$(BUILD_DIR)/loadtest/loadtest" "$(CURDIR)/$(PLUGIN)"

install: all
	@mkdir -p "$(PLUGIN_DIR)"
	install -m 0755 "$(PLUGIN)" "$(PLUGIN_DIR)/"
	@echo "installed to $(PLUGIN_DIR)/$(notdir $(PLUGIN))"

uninstall:
	rm -f "$(PLUGIN_DIR)/$(notdir $(PLUGIN))"

clean:
	rm -rf "$(BUILD_DIR)" build
