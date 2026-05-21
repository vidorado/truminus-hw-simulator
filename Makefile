PORT   ?= /dev/ttyACM0
TARGET ?= esp32c3

.PHONY: build flash monitor flash-monitor clean set-target

set-target:
	@idf.py set-target $(TARGET) >/dev/null 2>&1 || true

build: set-target
	idf.py build

flash:
	idf.py -p $(PORT) flash

monitor:
	idf.py -p $(PORT) monitor

flash-monitor:
	idf.py -p $(PORT) flash monitor

clean:
	idf.py fullclean
