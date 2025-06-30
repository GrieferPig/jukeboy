#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

// --- IMA ADPCM Constants ---
// This table provides the index adjustment for the step table.
static const int16_t INDEX_TABLE[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8};

// This is the full 89-entry table of quantization steps.
static const int16_t STEP_TABLE[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34,
    37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494,
    544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552,
    1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026,
    4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767};

// Holds the state (predictor and index) for a single ADPCM channel.
typedef struct
{
    int16_t predictor;
    int8_t index;
} AdpcmState;

// Clamps a value within a specified range.
int16_t clamp(int32_t value, int32_t min_val, int32_t max_val)
{
    if (value < min_val)
        return (int16_t)min_val;
    if (value > max_val)
        return (int16_t)max_val;
    return (int16_t)value;
}

// Encodes a single 16-bit PCM sample into a 4-bit ADPCM nibble.
uint8_t encode_sample(int16_t sample, AdpcmState *state)
{
    int32_t step = STEP_TABLE[state->index];
    int32_t diff = sample - state->predictor;
    uint8_t nibble = 0;

    if (diff < 0)
    {
        nibble = 8; // Set the sign bit
        diff = -diff;
    }

    // This logic quantizes the difference into a 4-bit nibble.
    int32_t vpdiff = step >> 3;
    if (diff >= step)
    {
        nibble |= 4;
        diff -= step;
        vpdiff += step;
    }
    step >>= 1;
    if (diff >= step)
    {
        nibble |= 2;
        diff -= step;
        vpdiff += step;
    }
    step >>= 1;
    if (diff >= step)
    {
        nibble |= 1;
        vpdiff += step;
    }

    // Update the predictor based on the calculated difference.
    if (nibble & 8)
    { // Check the sign bit
        state->predictor -= vpdiff;
    }
    else
    {
        state->predictor += vpdiff;
    }
    state->predictor = clamp(state->predictor, -32768, 32767);

    // Update the step index for the next sample.
    state->index += INDEX_TABLE[nibble];
    state->index = clamp(state->index, 0, 88);

    return nibble;
}

// Main function to process the files.
int main(int argc, char *argv[])
{
    if (argc != 4)
    {
        fprintf(stderr, "Usage: %s <input_pcm_file> <output_adpcm_file> <block_data_size>\n", argv[0]);
        return 1;
    }

    const char *pcm_path = argv[1];
    const char *adpcm_path = argv[2];
    const int block_data_size = atoi(argv[3]);
    // Each stereo sample pair (4 bytes) compresses to 1 byte of ADPCM data.
    const int pcm_chunk_size = block_data_size * 4;

    FILE *pcm_file = fopen(pcm_path, "rb");
    if (!pcm_file)
    {
        perror("Error opening input PCM file");
        return 1;
    }

    FILE *adpcm_file = fopen(adpcm_path, "wb");
    if (!adpcm_file)
    {
        perror("Error opening output ADPCM file");
        fclose(pcm_file);
        return 1;
    }

    int16_t *pcm_buffer = (int16_t *)malloc(pcm_chunk_size);
    uint8_t *adpcm_buffer = (uint8_t *)malloc(block_data_size);
    if (!pcm_buffer || !adpcm_buffer)
    {
        fprintf(stderr, "Failed to allocate memory\n");
        free(pcm_buffer);
        free(adpcm_buffer);
        fclose(pcm_file);
        fclose(adpcm_file);
        return 1;
    }

    AdpcmState state_l = {0, 0};
    AdpcmState state_r = {0, 0};
    size_t bytes_read;

    // Read the PCM file in chunks.
    while ((bytes_read = fread(pcm_buffer, 1, pcm_chunk_size, pcm_file)) > 0)
    {
        // Write the header for the current block. This state is from the *end* of the previous block.
        fwrite(&state_l.predictor, sizeof(int16_t), 1, adpcm_file);
        fwrite(&state_l.index, sizeof(int8_t), 1, adpcm_file);
        fwrite(&state_r.predictor, sizeof(int16_t), 1, adpcm_file);
        fwrite(&state_r.index, sizeof(int8_t), 1, adpcm_file);

        size_t num_stereo_samples = bytes_read / 4;
        for (size_t i = 0; i < num_stereo_samples; ++i)
        {
            int16_t sample_l = pcm_buffer[i * 2];
            int16_t sample_r = pcm_buffer[i * 2 + 1];

            uint8_t nibble_l = encode_sample(sample_l, &state_l);
            uint8_t nibble_r = encode_sample(sample_r, &state_r);

            adpcm_buffer[i] = (nibble_l << 4) | nibble_r;
        }

        // Write the encoded ADPCM data for the block.
        fwrite(adpcm_buffer, 1, num_stereo_samples, adpcm_file);

        // Pad the block if the last chunk was partial to maintain fixed block size.
        if (num_stereo_samples < block_data_size)
        {
            size_t padding_needed = block_data_size - num_stereo_samples;
            for (size_t p = 0; p < padding_needed; ++p)
            {
                fputc(0, adpcm_file);
            }
        }
    }

    printf("C Encoder: Processing complete.\n");

    // Cleanup
    free(pcm_buffer);
    free(adpcm_buffer);
    fclose(pcm_file);
    fclose(adpcm_file);

    return 0;
}
