# ESP32 DevKit V1 - build/flash helpers
#
#   make ports     list candidate serial ports
#   make build     compile only
#   make upload    compile + flash (auto-detects port)
#   make monitor   open serial monitor at 115200
#   make flash     upload, then immediately monitor
#   make clean     remove build artifacts
#   make intellisense  rebuild VSCode include paths
#   make remote    serve the Bluetooth phone remote on http://localhost:8000
#
# Override the port if auto-detect picks wrong:  make upload PORT=/dev/cu.usbserial-10

SKETCH   := .
# huge_app gives ~3 MB for the app instead of 1.3 MB -- BLE alone eats most
# of the default partition. No SPIFFS, which we do not use.
FQBN     := esp32:esp32:esp32:PartitionScheme=huge_app
BAUD     := 115200
UPSPEED  := 921600
BUILDDIR := build

# Prefer a real USB-serial device; ignore Bluetooth / debug-console ports.
PORT ?= $(firstword $(wildcard /dev/cu.usbserial-* /dev/cu.wchusbserial* \
             /dev/cu.usbmodem* /dev/cu.SLAB_USBtoUART*))

.PHONY: ports build upload monitor flash clean check-port intellisense remote

ports:
	@echo "Candidate ESP32 ports:"
	@for p in $(wildcard /dev/cu.usbserial-* /dev/cu.wchusbserial* \
	            /dev/cu.usbmodem* /dev/cu.SLAB_USBtoUART*); do echo "  $$p"; done
	@test -n "$(PORT)" || echo "  (none found - is the board plugged in?)"
	@echo
	@echo "All serial devices:"
	@ls /dev/cu.* 2>/dev/null
	@echo
	@arduino-cli board list

check-port:
	@if [ -z "$(PORT)" ]; then \
	  echo "ERROR: no ESP32 serial port found."; \
	  echo "  - plug the board in with the USB-C cable (must be a DATA cable, not charge-only)"; \
	  echo "  - run 'make ports' to see what macOS sees"; \
	  echo "  - if still nothing, you likely need a CP210x or CH34x USB-serial driver"; \
	  exit 1; \
	fi
	@echo "Using port: $(PORT)"

build:
	arduino-cli compile --fqbn $(FQBN) --build-path $(BUILDDIR) --warnings default $(SKETCH)

upload: check-port
	arduino-cli compile --fqbn $(FQBN) --build-path $(BUILDDIR) $(SKETCH)
	arduino-cli upload --fqbn $(FQBN) --port $(PORT) --input-dir $(BUILDDIR) \
	  --upload-field upload_speed=$(UPSPEED) $(SKETCH) || \
	  arduino-cli upload --fqbn $(FQBN) --port $(PORT) --input-dir $(BUILDDIR) $(SKETCH)

monitor: check-port
	@echo "Ctrl-C to exit the monitor."
	arduino-cli monitor --port $(PORT) --config baudrate=$(BAUD)

flash: upload monitor

# Regenerate VSCode IntelliSense paths from the real compile flags.
# Re-run after upgrading the ESP32 core or installing a new library.
intellisense: build
	python3 tools/gen_intellisense.py

# Web Bluetooth needs https or localhost, so serve the page locally for
# testing from Chrome on this Mac. Ctrl-C to stop.
remote:
	@echo "Open http://localhost:8000 in Chrome. Ctrl-C to stop."
	cd remote && python3 -m http.server 8000

clean:
	rm -rf $(BUILDDIR)
