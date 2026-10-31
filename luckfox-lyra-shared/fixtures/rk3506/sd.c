/* sd.c — on this board, the SD card takes the place of the WeAct libspi's SPI flash. Main owns
 * the card, and this library imports reads and writes of one file on its FAT volume, so nothing
 * else on the card is touched. */
#include <stdint.h>

extern int board_sd_store(const void* data, uint32_t len);
extern int board_sd_load(void* data, uint32_t len);

static int initialized = 0;
static uint32_t transfers = 0;
static uint8_t out[512], in[512];

void sd_init(void) { initialized = 1; }

/* Write a pattern from seed, read it back, and return how many bytes came back different. */
uint32_t sd_selftest(uint32_t seed, uint32_t len) {
    if (!initialized || len > sizeof out) return len;
    uint8_t v = (uint8_t)seed;
    for (uint32_t i = 0; i < len; i++, v = (uint8_t)(v * 31u + 7u)) { out[i] = v; in[i] = (uint8_t)~v; }
    if (!board_sd_store(out, len) || !board_sd_load(in, len)) return len;
    uint32_t bad = 0;
    for (uint32_t i = 0; i < len; i++) {
        if (in[i] != out[i]) ++bad;
        ++transfers;
    }
    return bad;
}

uint32_t sd_transfers(uint32_t a, uint32_t b) { (void)a; (void)b; return transfers; }

void sd_discard(void) { initialized = 0; }
