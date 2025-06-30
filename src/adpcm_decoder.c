#include "adpcm_decoder.h"
#include <string.h> // For memcpy

// --- IMA ADPCM Constants ---
static const int16_t INDEX_TABLE[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8};

static const int16_t STEP_TABLE[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34,
    37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494,
    544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552,
    1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026,
    4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767};

// --- Internal Decoder State ---
typedef struct
{
    int16_t predictor;
    int8_t index;
} AdpcmState;

// Helper function to clamp a value within a specified range.
static int16_t clamp(int32_t val, int32_t min, int32_t max)
{
    if (val < min)
        return (int16_t)min;
    if (val > max)
        return (int16_t)max;
    return (int16_t)val;
}

// Helper function to decode a single 4-bit nibble.
static int16_t decode_sample(uint8_t nibble, AdpcmState *state)
{
    int32_t step = STEP_TABLE[state->index];
    int32_t vpdiff = step >> 3;

    if (nibble & 4)
        vpdiff += step;
    if (nibble & 2)
        vpdiff += step >> 1;
    if (nibble & 1)
        vpdiff += step >> 2;

    if (nibble & 8)
    { // sign bit
        state->predictor -= vpdiff;
    }
    else
    {
        state->predictor += vpdiff;
    }
    state->predictor = clamp(state->predictor, -32768, 32767);

    state->index += INDEX_TABLE[nibble];
    state->index = clamp(state->index, 0, 88);

    return state->predictor;
}

// --- Public API Function ---
size_t decode_adpcm_block(const uint8_t *adpcm_block, int16_t *pcm_output_buffer, size_t pcm_output_buffer_size)
{
    const uint32_t ADPCM_BLOCK_HEADER_SIZE = 6;
    const uint32_t ADPCM_BLOCK_DATA_SIZE = 44032 - ADPCM_BLOCK_HEADER_SIZE;

    // Initialize decoder states from the 6-byte block header
    AdpcmState state_l, state_r;
    memcpy(&state_l.predictor, adpcm_block, 2);
    memcpy(&state_l.index, adpcm_block + 2, 1);
    memcpy(&state_r.predictor, adpcm_block + 3, 2);
    memcpy(&state_r.index, adpcm_block + 5, 1);

    const uint8_t *adpcm_data = adpcm_block + ADPCM_BLOCK_HEADER_SIZE;
    size_t pcm_bytes_written = 0;

    // Loop through the data portion of the block
    for (size_t i = 0; i < ADPCM_BLOCK_DATA_SIZE; ++i)
    {
        // Ensure we don't write past the end of the output buffer
        if (pcm_bytes_written + 4 > pcm_output_buffer_size)
        {
            break;
        }

        uint8_t adpcm_byte = adpcm_data[i];

        // First nibble is for the Left channel
        uint8_t nibble_l = (adpcm_byte >> 4) & 0x0F;
        pcm_output_buffer[pcm_bytes_written / 2] = decode_sample(nibble_l, &state_l);

        // Second nibble is for the Right channel
        uint8_t nibble_r = adpcm_byte & 0x0F;
        pcm_output_buffer[pcm_bytes_written / 2 + 1] = decode_sample(nibble_r, &state_r);

        pcm_bytes_written += 4; // Wrote 2 stereo samples (4 bytes)
    }

    return pcm_bytes_written;
}
