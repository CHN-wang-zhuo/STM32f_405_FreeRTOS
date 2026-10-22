#include "fw_update.h"
#include "fw_update_port.h"
#include <string.h>

static bool ValidManifest(const BootImageManifest *m)
{
    return m->magic == BOOT_IMAGE_MAGIC && m->format == BOOT_IMAGE_FORMAT &&
           m->commit == BOOT_IMAGE_COMMITTED && m->consumed == BOOT_ERASED_WORD &&
           m->image_size >= 8U && m->image_size <= BOOT_IMAGE_CAPACITY &&
           m->header_crc == Update_Crc32(m, 20U);
}

static bool ValidVectors(const uint8_t *image, uint32_t size)
{
    const uint32_t sp = Update_Get32(image);
    const uint32_t entry = Update_Get32(image + 4);
    const uint32_t pc = entry & ~UINT32_C(1);
    return (sp & 7U) == 0U &&
           ((sp > UINT32_C(0x20000000) && sp <= UINT32_C(0x20020000)) ||
            (sp > UINT32_C(0x10000000) && sp <= UINT32_C(0x10010000))) &&
           (entry & 1U) != 0U && pc >= BOOT_APP_ADDRESS + 8U && pc < BOOT_APP_ADDRESS + size;
}

void FwUpdate_Init(FwUpdate *u)
{
    memset(u, 0, sizeof(*u));
    BootImageManifest installed;
    FwPort_ReadInstalled(&installed);
    if (ValidManifest(&installed)) { u->running_version = installed.version; }
    BootImageManifest pending;
    FwPort_ReadRequest(&pending);
    if (ValidManifest(&pending)) {
        const uint8_t *image = FwPort_Stage(0, pending.image_size);
        if (image != NULL && ValidVectors(image, pending.image_size) &&
            Update_Crc32(image, pending.image_size) == pending.image_crc) {
            u->image = pending;
            u->received = pending.image_size;
            u->state = UPDATE_PENDING;
        }
    }
}

void FwUpdate_Expire(FwUpdate *u)
{
    if (u->state == UPDATE_RECEIVING) {
        u->state = UPDATE_IDLE;
        u->received = 0;
        u->cached = false; /* Request is still invalid; old APP is untouched. */
    }
}

static uint8_t Begin(FwUpdate *u, const UpdateFrame *f)
{
    if (f->length != 12U) { return UPDATE_BAD_LENGTH; }
    const uint32_t size = Update_Get32(f->payload);
    if (size < 8U || size > BOOT_IMAGE_CAPACITY) { return UPDATE_BAD_LENGTH; }
    /* A fresh BEGIN explicitly replaces any previously deferred request. */
    if (!FwPort_Begin()) { u->state = UPDATE_IDLE; return UPDATE_FLASH_ERROR; }
    memset(&u->image, 0xFF, sizeof(u->image));
    u->image.magic = BOOT_IMAGE_MAGIC;
    u->image.format = BOOT_IMAGE_FORMAT;
    u->image.image_size = size;
    u->image.image_crc = Update_Get32(f->payload + 4);
    u->image.version = Update_Get32(f->payload + 8);
    u->image.header_crc = Update_Crc32(&u->image, 20U);
    u->image.commit = BOOT_IMAGE_COMMITTED;
    u->received = 0;
    u->state = UPDATE_RECEIVING;
    return UPDATE_OK;
}

static uint8_t Data(FwUpdate *u, const UpdateFrame *f)
{
    if (u->state != UPDATE_RECEIVING) { return UPDATE_BAD_STATE; }
    if (f->length <= 4U || f->length > UPDATE_MAX_PAYLOAD) { return UPDATE_BAD_LENGTH; }
    const uint32_t offset = Update_Get32(f->payload);
    const uint32_t count = f->length - 4U;
    if ((offset & 3U) != 0U || offset > u->received || offset > u->image.image_size ||
        count > u->image.image_size - offset ||
        ((count & 3U) != 0U && offset + count != u->image.image_size)) { return UPDATE_BAD_OFFSET; }
    if (offset < u->received) {
        const uint8_t *stored = FwPort_Stage(offset, count);
        return offset + count <= u->received && stored != NULL &&
               memcmp(stored, f->payload + 4, count) == 0 ? UPDATE_OK : UPDATE_BAD_OFFSET;
    }
    if (!FwPort_Write(offset, f->payload + 4, count)) {
        u->state = UPDATE_IDLE;
        return UPDATE_FLASH_ERROR;
    }
    u->received += count;
    return UPDATE_OK;
}

static uint8_t Finish(FwUpdate *u, const UpdateFrame *f, bool *reset)
{
    if (f->length != 1U || f->payload[0] > 1U) { return UPDATE_BAD_LENGTH; }
    if (u->state != UPDATE_RECEIVING || u->received != u->image.image_size) { return UPDATE_BAD_STATE; }
    const uint8_t *image = FwPort_Stage(0, u->image.image_size);
    if (image == NULL || Update_Crc32(image, u->image.image_size) != u->image.image_crc) { return UPDATE_BAD_CRC; }
    if (!ValidVectors(image, u->image.image_size)) { return UPDATE_BAD_IMAGE; }
    if (!FwPort_Commit(&u->image)) { u->state = UPDATE_IDLE; return UPDATE_FLASH_ERROR; }
    u->state = UPDATE_PENDING;
    *reset = f->payload[0] == 1U;
    return UPDATE_OK;
}

void FwUpdate_Process(FwUpdate *u, const UpdateFrame *f, FwUpdateReply *reply)
{
    const uint32_t crc = Update_Crc32(f->payload, f->length);
    uint8_t status = UPDATE_OK;
    bool reset = false;
    if (u->cached && f->sequence == u->last_sequence) {
        if (f->command == u->last_command && f->length == u->last_length && crc == u->last_crc) {
            *reply = u->last_reply;
            return;
        }
        status = UPDATE_BAD_SEQUENCE;
    } else {
        switch (f->command) {
        case UPDATE_QUERY:
            status = f->length == 0U ? UPDATE_OK : UPDATE_BAD_LENGTH;
            break;
        case UPDATE_BEGIN: status = Begin(u, f); break;
        case UPDATE_DATA: status = Data(u, f); break;
        case UPDATE_END: status = Finish(u, f, &reset); break;
        case UPDATE_ABORT:
            if (f->length != 0U) { status = UPDATE_BAD_LENGTH; }
            else if (!FwPort_Cancel()) { status = UPDATE_FLASH_ERROR; }
            else { u->state = UPDATE_IDLE; u->received = 0; memset(&u->image, 0, sizeof(u->image)); }
            break;
        case UPDATE_REBOOT:
            if (f->length != 0U) { status = UPDATE_BAD_LENGTH; }
            else if (u->state != UPDATE_PENDING) { status = UPDATE_BAD_STATE; }
            else { reset = true; }
            break;
        default: status = UPDATE_BAD_COMMAND; break;
        }
    }
    reply->bytes[0] = f->command;
    reply->bytes[1] = status;
    reply->bytes[2] = u->state;
    reply->bytes[3] = reset ? 1U : 0U;
    Update_Put32(reply->bytes + 4, u->received);
    Update_Put32(reply->bytes + 8, u->image.image_size);
    Update_Put32(reply->bytes + 12, u->image.version);
    Update_Put32(reply->bytes + 16, u->image.image_crc);
    Update_Put32(reply->bytes + 20, UPDATE_CHUNK_SIZE);
    Update_Put32(reply->bytes + 24, u->running_version);
    reply->reset_after_ack = reset;
    if (status != UPDATE_BAD_SEQUENCE) {
        u->last_command = f->command;
        u->last_sequence = f->sequence;
        u->last_length = f->length;
        u->last_crc = crc;
        u->last_reply = *reply;
        u->cached = true;
    }
}
