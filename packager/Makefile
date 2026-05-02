# GBM Packager Build

CC = gcc
CC_WIN = x86_64-w64-mingw32-gcc
CFLAGS = -O2 -Wall

GBA_ROM = ../M3_Movie_Player.gba

all: gbm_packager gbm_packager.exe mb_video_packager

linux: gbm_packager

multibank: mb_video_packager

windows: gbm_packager.exe

embedded_data.h: $(GBA_ROM)
	@echo "Generating embedded_data.h..."
	@echo "// Auto-generated - do not edit" > $@
	@echo "#include <stdint.h>" >> $@
	@echo "static const uint32_t embedded_gba_size = $$(wc -c < $(GBA_ROM));" >> $@
	@echo "static const uint8_t embedded_gba_data[] = {" >> $@
	@xxd -i < $(GBA_ROM) >> $@
	@echo "};" >> $@

gbm_packager: gbm_packager.c embedded_data.h
	$(CC) $(CFLAGS) -o $@ gbm_packager.c

mb_video_packager: mb_video_packager.c
	$(CC) $(CFLAGS) -o $@ mb_video_packager.c

gbm_packager.exe: gbm_packager.c embedded_data.h
	$(CC_WIN) $(CFLAGS) -o $@ gbm_packager.c

clean:
	rm -f gbm_packager gbm_packager.exe mb_video_packager embedded_data.h

.PHONY: all linux windows multibank clean
