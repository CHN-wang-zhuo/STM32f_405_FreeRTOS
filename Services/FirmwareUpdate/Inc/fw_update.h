#ifndef FW_UPDATE_H
#define FW_UPDATE_H
#include "boot_protocol.h"
#include "update_protocol.h"

typedef struct {
    uint8_t bytes[UPDATE_RESPONSE_SIZE];
    bool reset_after_ack;
} FwUpdateReply;

typedef struct {
    uint8_t state;
    bool storage_ready;
    uint32_t received;
    uint32_t running_version;
    BootImageManifest image;
    bool cached;
    uint8_t last_command;
    uint16_t last_length;
    uint32_t last_sequence;
    uint32_t last_crc;
    FwUpdateReply last_reply;
} FwUpdate;

void FwUpdate_Init(FwUpdate *update);
void FwUpdate_Expire(FwUpdate *update);
void FwUpdate_Process(FwUpdate *update, const UpdateFrame *frame, FwUpdateReply *reply);
#endif
