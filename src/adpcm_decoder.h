#ifndef ADPCM_DECODER_H
#define ADPCM_DECODER_H

#include <stdint.h>
#include <stddef.h> // For size_t

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Decodes a single block of dual-state stereo IMA ADPCM data into raw PCM data.
     *
     * This function is designed to be self-contained. It reads the 6-byte header from the
     * start of the ADPCM block to initialize its internal state for decoding.
     *
     * @param adpcm_block A pointer to the input buffer containing one full ADPCM block
     * (header + data). The size must be ADPCM_BLOCK_SIZE.
     * @param pcm_output_buffer A pointer to the output buffer where the decoded, interleaved
     * 16-bit stereo PCM samples will be written.
     * @param pcm_output_buffer_size The total size in bytes of the output buffer. This is used
     * to prevent buffer overflows.
     * @return The number of raw PCM bytes written to the output buffer. This will typically be
     * (ADPCM_BLOCK_SIZE - 6) * 4.
     */
    size_t decode_adpcm_block(const uint8_t *adpcm_block, int16_t *pcm_output_buffer, size_t pcm_output_buffer_size);

#ifdef __cplusplus
}
#endif

#endif // ADPCM_DECODER_H
