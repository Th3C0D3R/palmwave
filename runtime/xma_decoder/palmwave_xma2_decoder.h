#ifndef PALMWAVE_XMA2_DECODER_H
#define PALMWAVE_XMA2_DECODER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PALMWAVE_XMA2_PACKET_BYTES 2048u

typedef struct PalmwaveXma2Decoder PalmwaveXma2Decoder;
typedef struct PalmwaveXma2PcmBlock PalmwaveXma2PcmBlock;

enum PalmwaveXma2Status {
  PALMWAVE_XMA2_OK = 0,
  PALMWAVE_XMA2_INVALID_ARGUMENT = 1,
  PALMWAVE_XMA2_INVALID_FORMAT = 2,
  PALMWAVE_XMA2_BAD_PACKET_SIZE = 3,
  PALMWAVE_XMA2_OUT_OF_MEMORY = 4,
  PALMWAVE_XMA2_DECODER_FAILURE = 5,
  PALMWAVE_XMA2_PANIC = 6,
};

int32_t palmwave_xma2_decoder_create(uint32_t sample_rate, uint32_t channels,
                                    PalmwaveXma2Decoder **out_decoder);
int32_t palmwave_xma2_decode_buffer(PalmwaveXma2Decoder *decoder,
                                   const uint8_t *input, size_t input_len,
                                   PalmwaveXma2PcmBlock **out_block);
void palmwave_xma2_decoder_destroy(PalmwaveXma2Decoder *decoder);
const int16_t *palmwave_xma2_pcm_data(const PalmwaveXma2PcmBlock *block);
size_t palmwave_xma2_pcm_sample_count(const PalmwaveXma2PcmBlock *block);
void palmwave_xma2_pcm_destroy(PalmwaveXma2PcmBlock *block);

#ifdef __cplusplus
}
#endif

#endif
