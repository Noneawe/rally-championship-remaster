#include "stage.h"

static uint8_t fog_noise[STAGE_FOG_NOISE_N];
static int fog_noise_ready;

static uint32_t fog_lfsr(uint32_t state)
{
    uint32_t low = state & 0xffu;

    low ^= (state & 0x00400000u) ? 1u : 0u;
    low ^= (low & 0x00000004u) ? 1u : 0u;
    low ^= (low & 0x00000002u) ? 1u : 0u;
    state = (state & 0xffffff00u) | (low & 0xffu);
    state = (state >> 1) | (state << 31);
    return state ^ 0x87654321u;
}

const uint8_t *stage_fog_noise(void)
{
    if (!fog_noise_ready) {
        uint32_t state = 0x0012d687u;
        int i;

        for (i = 0; i < STAGE_FOG_NOISE_N; i++) {
            state = fog_lfsr(state);
            fog_noise[i] = (uint8_t)((state >> 12) & 0xffu);
        }
        fog_noise_ready = 1;
    }
    return fog_noise;
}