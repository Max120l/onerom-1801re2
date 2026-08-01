# Host build of the decode tests.  No SDK, no cross compiler -- decode.c is
# deliberately free of hardware dependencies so this is just cc.

CFLAGS ?= -std=c11 -O1 -g -Wall -Wextra -Wshadow -I../firmware
BUILD  := build

$(BUILD)/test_decode: test_decode.c ../firmware/decode.c ../firmware/decode.h \
                      ../firmware/board_fire24e.h ../firmware/rom_images.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_decode.c ../firmware/decode.c

.PHONY: check clean
check: $(BUILD)/test_decode
	./$(BUILD)/test_decode

clean:
	rm -rf $(BUILD)
