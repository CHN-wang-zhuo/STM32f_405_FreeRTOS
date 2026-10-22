#ifndef UPDATE_PROTOCOL_H
#define UPDATE_PROTOCOL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UPDATE_PROTOCOL_VERSION 1U
#define UPDATE_CHUNK_SIZE 1024U
#define UPDATE_MAX_PAYLOAD (UPDATE_CHUNK_SIZE + 4U)
#define UPDATE_HEADER_SIZE 16U
#define UPDATE_MAX_FRAME (UPDATE_HEADER_SIZE + UPDATE_MAX_PAYLOAD)
#define UPDATE_RESPONSE_SIZE 28U

enum { UPDATE_QUERY = 1, UPDATE_BEGIN, UPDATE_DATA, UPDATE_END, UPDATE_ABORT, UPDATE_REBOOT };
enum { UPDATE_OK = 0, UPDATE_BAD_COMMAND, UPDATE_BAD_LENGTH, UPDATE_BAD_STATE,
       UPDATE_BAD_OFFSET, UPDATE_BAD_CRC, UPDATE_FLASH_ERROR, UPDATE_BAD_IMAGE,
       UPDATE_BAD_SEQUENCE };
enum { UPDATE_IDLE = 0, UPDATE_RECEIVING, UPDATE_PENDING };

typedef struct {
    uint8_t command;
    uint32_t sequence;
    uint16_t length;
    const uint8_t *payload;
} UpdateFrame;

typedef struct {
    uint8_t bytes[UPDATE_MAX_FRAME];
    size_t used;
    size_t expected;
} UpdateDecoder;

typedef enum { UPDATE_INCOMPLETE, UPDATE_FRAME_READY, UPDATE_FRAME_REJECTED } UpdateDecodeResult;

uint16_t Update_Get16(const uint8_t *p);
uint32_t Update_Get32(const uint8_t *p);
void Update_Put32(uint8_t *p, uint32_t value);
uint32_t Update_CrcContinue(uint32_t state, const void *data, size_t size);
uint32_t Update_Crc32(const void *data, size_t size);
void UpdateDecoder_Reset(UpdateDecoder *decoder);
UpdateDecodeResult UpdateDecoder_Push(UpdateDecoder *decoder, uint8_t byte, UpdateFrame *frame);
size_t UpdateProtocol_Encode(uint8_t *out, size_t capacity, uint8_t command,
                             uint32_t sequence, const void *payload, uint16_t length);
#endif
