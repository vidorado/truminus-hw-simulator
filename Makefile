PORT   ?= /dev/ttyACM0
TARGET ?= esp32c3

.PHONY: build flash monitor flash-monitor clean set-target \
        openair openair-flash openair-flash-monitor

set-target:
	@idf.py set-target $(TARGET) >/dev/null 2>&1 || true

build: set-target
	idf.py -DOPENAIR_SIM=0 build

# OpenAir PLUS capture personality (advertises as "OpenAir PLUS", logs every
# command the app writes).  Switching personality flips a compile define, so
# `idf.py` reconfigures automatically — no `make clean` needed.
openair: set-target
	idf.py -DOPENAIR_SIM=1 build

openair-flash: openair
	idf.py -p $(PORT) -DOPENAIR_SIM=1 flash

openair-flash-monitor: openair
	idf.py -p $(PORT) -DOPENAIR_SIM=1 flash monitor

flash:
	idf.py -p $(PORT) flash

monitor:
	idf.py -p $(PORT) monitor

flash-monitor:
	idf.py -p $(PORT) flash monitor

clean:
	idf.py fullclean
