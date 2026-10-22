#include "update_protocol.h"
#include <string.h>

static const uint8_t magic[4] = {'F', 'W', 'U', 'P'};

uint16_t Update_Get16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

uint32_t Update_Get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void Update_Put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) { p[i] = (uint8_t)(value >> (8U * i)); }
}

uint32_t Update_CrcContinue(uint32_t state, const void *data, size_t size)
{
    const uint8_t *p = data;
    for (size_t i = 0; i < size; ++i) {
        state ^= p[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            state = (state >> 1) ^ ((0U - (state & 1U)) & UINT32_C(0xEDB88320));
        }
    }
    return state;
}

uint32_t Update_Crc32(const void *data, size_t size)
{
    return ~Update_CrcContinue(UINT32_MAX, data, size);
}

void UpdateDecoder_Reset(UpdateDecoder *decoder)
{
    decoder->used = 0;
    decoder->expected = UPDATE_HEADER_SIZE;
}

UpdateDecodeResult UpdateDecoder_Push(UpdateDecoder *d, uint8_t byte, UpdateFrame *frame)
{
    if (d->used < 4U && byte != magic[d->used]) {
        d->used = byte == magic[0] ? 1U : 0U;
        if (d->used != 0U) { d->bytes[0] = byte; }
        return UPDATE_INCOMPLETE;
    }
    d->bytes[d->used++] = byte;
    if (d->used == UPDATE_HEADER_SIZE) {
        const uint16_t length = Update_Get16(d->bytes + 6);
        if (d->bytes[4] != UPDATE_PROTOCOL_VERSION || length > UPDATE_MAX_PAYLOAD) {
            UpdateDecoder_Reset(d);
            return UPDATE_FRAME_REJECTED;
        }
        d->expected = UPDATE_HEADER_SIZE + length;
    }
    if (d->used < d->expected) { return UPDATE_INCOMPLETE; }
    uint32_t crc = Update_CrcContinue(UINT32_MAX, d->bytes + 4, 8);
    crc = ~Update_CrcContinue(crc, d->bytes + UPDATE_HEADER_SIZE,
                              d->expected - UPDATE_HEADER_SIZE);
    const bool valid = crc == Update_Get32(d->bytes + 12);
    frame->command = d->bytes[5];
    frame->sequence = Update_Get32(d->bytes + 8);
    frame->length = Update_Get16(d->bytes + 6);
    frame->payload = d->bytes + UPDATE_HEADER_SIZE;
    UpdateDecoder_Reset(d); /* Payload remains valid until the next Push. */
    return valid ? UPDATE_FRAME_READY : UPDATE_FRAME_REJECTED;
}

size_t UpdateProtocol_Encode(uint8_t *out, size_t capacity, uint8_t command,
                             uint32_t sequence, const void *payload, uint16_t length)
{
    if (length > UPDATE_MAX_PAYLOAD || capacity < UPDATE_HEADER_SIZE + (size_t)length) { return 0; }
    memcpy(out, magic, sizeof(magic));
    out[4] = UPDATE_PROTOCOL_VERSION;
    out[5] = command;
    out[6] = (uint8_t)length;
    out[7] = (uint8_t)(length >> 8);
    Update_Put32(out + 8, sequence);
    if (length != 0U) { memcpy(out + UPDATE_HEADER_SIZE, payload, length); }
    uint32_t crc = Update_CrcContinue(UINT32_MAX, out + 4, 8);
    crc = ~Update_CrcContinue(crc, out + UPDATE_HEADER_SIZE, length);
    Update_Put32(out + 12, crc);
    return UPDATE_HEADER_SIZE + (size_t)length;
}
