#ifndef ADPCM_DECODER_H
#define ADPCM_DECODER_H

#include <stdint.h>
#include <stddef.h> // For size_t

#ifdef __cplusplus
extern "C"
{
#endif

    // --- Internal Decoder State ---
    // This is now exposed in the header so the context can hold it.
    typedef struct
    {
        int16_t predictor;
        int8_t index;
    } AdpcmState;

    // --- NEW: Stateful Decoder Context ---
    // This structure holds the entire state of a block decoding operation.
    typedef struct
    {
        const uint8_t *adpcm_data_ptr; // Pointer to the start of the ADPCM data (after the header)
        size_t adpcm_data_len;         // Total length of the ADPCM data in the block
        size_t adpcm_read_pos;         // Current read position within the ADPCM data
        AdpcmState state_l;            // Current state for the left channel
        AdpcmState state_r;            // Current state for the right channel
    } DecoderContext;

    /**
     * @brief Initializes a decoder context with the state from a new ADPCM block.
     *
     * Call this once at the beginning of each new 44KB ADPCM block. It reads the
     * 6-byte header and prepares the context for decoding.
     *
     * @param ctx Pointer to the DecoderContext to initialize.
     * @param adpcm_block Pointer to the start of the full ADPCM block (including header).
     * @param adpcm_block_size The total size of the block (e.g., 44032 bytes).
     */
    void adpcm_decoder_init(DecoderContext *ctx, const uint8_t *adpcm_block, size_t adpcm_block_size);

    /**
     * @brief Decodes the next chunk of ADPCM data from the context into a PCM buffer.
     *
     * Call this repeatedly to fill a small PCM buffer. The function will update the
     * context's internal state, remembering its position for the next call.
     *
     * @param ctx Pointer to the initialized and active DecoderContext.
     * @param pcm_output_buffer Pointer to the buffer where decoded PCM samples will be written.
     * @param pcm_output_buffer_size The size in bytes of the output buffer.
     * @return The number of PCM bytes written to the output buffer. Returns 0 if the
     * end of the ADPCM block data has been reached.
     */
    size_t adpcm_decode_chunk(DecoderContext *ctx, int16_t *pcm_output_buffer, size_t pcm_output_buffer_size);

#ifdef __cplusplus
}
#endif

#endif // ADPCM_DECODER_H
