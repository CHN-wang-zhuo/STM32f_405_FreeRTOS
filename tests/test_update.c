/* Actual protocol, service, external storage backend and UNCHANGED Bootloader.
 * The W25Q16 model enforces NOR programming and injects partial writes. */
#include "fw_update.h"
#include "fw_update_port.h"
#include "update_storage_layout.h"
#include "w25q16.h"
#include "bootloader.h"
#include "boot_port.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t internal[BOOT_FLASH_SIZE], external[W25Q16_CAPACITY], image[67];
static unsigned operation, fail_at, erases, writes, reads;
static uint32_t crc_state;
static bool unavailable, fail_read;
volatile uint32_t g_w25q16_jedec_id, g_w25q16_errors;
static uint8_t *At(uint32_t address) { assert(address >= BOOT_FLASH_BASE && address < BOOT_FLASH_BASE + BOOT_FLASH_SIZE); return internal + address - BOOT_FLASH_BASE; }
static bool Step(void) { return ++operation != fail_at; }
bool W25Q16_Init(void) { return !unavailable; }
bool W25Q16_Read(uint32_t address, void *data, uint32_t size)
{
    assert(address < sizeof(external) && size <= sizeof(external) - address);
    ++reads;
    if (unavailable || fail_read) { return false; }
    memcpy(data, external + address, size); return true;
}
bool W25Q16_EraseSector(uint32_t address)
{
    assert((address % W25Q16_SECTOR_SIZE) == 0);
    assert(address == UPDATE_EXT_REQUEST_OFFSET ||
           (address >= UPDATE_EXT_IMAGE_OFFSET && address < UPDATE_EXT_IMAGE_OFFSET + UPDATE_EXT_IMAGE_CAPACITY));
    if (address != UPDATE_EXT_REQUEST_OFFSET) {
        assert(Update_Get32(external + 24) == UINT32_MAX); /* Cancel request FIRST. */
    }
    if (!Step()) { return false; }
    memset(external + address, 0xFF, W25Q16_SECTOR_SIZE); ++erases; return true;
}
bool W25Q16_Program(uint32_t address, const void *data, uint32_t size)
{
    assert((address < sizeof(UpdateStorageRecord) && size <= sizeof(UpdateStorageRecord) - address) ||
           (address >= UPDATE_EXT_IMAGE_OFFSET && address + size <= UPDATE_EXT_IMAGE_OFFSET + UPDATE_EXT_IMAGE_CAPACITY));
    const uint8_t *source = data;
    for (uint32_t i = 0; i < size; ++i) {
        if (!Step()) { return false; } /* Partial programming, including commit. */
        assert((external[address + i] & source[i]) == source[i]);
        external[address + i] &= source[i]; ++writes;
    }
    return true;
}
void FwPort_ReadInstalled(BootImageManifest *m) { memcpy(m, At(BOOT_INSTALLED_ADDRESS), sizeof(*m)); }

const void *BootPort_Map(uint32_t address) { return At(address); }
void BootPort_CrcBegin(void) { crc_state = UINT32_MAX; }
void BootPort_CrcFeedWord(uint32_t word) { crc_state = Update_CrcContinue(crc_state, &word, 4); }
uint32_t BootPort_CrcFinish(void) { return crc_state; }
bool BootPort_Unlock(void) { assert(!"external staging must not trigger old Bootloader writes"); return false; }
void BootPort_Lock(void) { assert(!"unexpected internal Flash operation"); }
bool BootPort_EraseInstalled(void) { assert(!"unexpected internal Flash erase"); return false; }
bool BootPort_EraseApplication(void) { assert(!"unexpected internal Flash erase"); return false; }
bool BootPort_ProgramWord(uint32_t address, uint32_t value) { (void)address; (void)value; assert(!"unexpected internal Flash program"); return false; }

static void Setup(void)
{
    memset(internal, 0xA5, sizeof(internal));
    memset(At(BOOT_REQUEST_ADDRESS), 0xFF, 0x4000);
    memset(external, 0xA5, sizeof(external));
    memset(external + UPDATE_EXT_REQUEST_OFFSET, 0xFF, W25Q16_SECTOR_SIZE);
    for (unsigned i = 0; i < sizeof(image); ++i) { image[i] = (uint8_t)(i * 7); }
    Update_Put32(image, 0x20020000); Update_Put32(image + 4, BOOT_APP_ADDRESS + 9);
    memcpy(At(BOOT_APP_ADDRESS), image, sizeof(image));
    BootImageManifest m = {BOOT_IMAGE_MAGIC, BOOT_IMAGE_FORMAT, sizeof(image),
                          0, 1, 0, BOOT_IMAGE_COMMITTED, BOOT_ERASED_WORD};
    m.image_crc = Update_Crc32(image, sizeof(image)); m.header_crc = Update_Crc32(&m, 20);
    memcpy(At(BOOT_INSTALLED_ADDRESS), &m, sizeof(m));
    operation = fail_at = erases = writes = reads = 0;
    unavailable = fail_read = false;
}
static FwUpdateReply Request(FwUpdate *u, uint8_t command, uint32_t sequence, const void *data, uint16_t length)
{
    const UpdateFrame f = {command, sequence, length, data}; FwUpdateReply r;
    FwUpdate_Process(u, &f, &r); return r;
}
static FwUpdateReply Begin(FwUpdate *u, uint32_t sequence, const uint8_t *data, uint32_t size)
{
    uint8_t begin[12]; Update_Put32(begin, size);
    Update_Put32(begin + 4, Update_Crc32(data, size)); Update_Put32(begin + 8, 2);
    return Request(u, UPDATE_BEGIN, sequence, begin, sizeof(begin));
}
static bool Transfer(FwUpdate *u, const uint8_t *source, uint32_t size, uint8_t mode)
{
    FwUpdateReply r = Begin(u, 10, source, size);
    if (r.bytes[1] != UPDATE_OK) { return false; }
    uint8_t data[UPDATE_MAX_PAYLOAD]; uint32_t sequence = 11;
    for (uint32_t offset = 0; offset < size; offset += UPDATE_CHUNK_SIZE) {
        const uint32_t count = size - offset < UPDATE_CHUNK_SIZE ? size - offset : UPDATE_CHUNK_SIZE;
        Update_Put32(data, offset); memcpy(data + 4, source + offset, count);
        r = Request(u, UPDATE_DATA, sequence++, data, (uint16_t)(count + 4));
        if (r.bytes[1] != UPDATE_OK || u->received != offset + count) { return false; }
    }
    r = Request(u, UPDATE_END, sequence, &mode, 1);
    if (r.bytes[1] != UPDATE_OK) { return false; }
    assert(r.reset_after_ack == (mode == 1)); return true;
}
static void Unchanged(void)
{
    assert(Boot_PrepareApplication() == BOOT_READY);
    assert(Update_Get32(At(BOOT_INSTALLED_ADDRESS + 16)) == 1);
    assert(memcmp(At(BOOT_APP_ADDRESS), image, sizeof(image)) == 0);
    for (uint32_t i = 0; i < BOOT_LOADER_SIZE; ++i) { assert(internal[i] == 0xA5); }
    for (uint32_t i = BOOT_STAGE_ADDRESS - BOOT_FLASH_BASE; i < sizeof(internal); ++i) { assert(internal[i] == 0xA5); }
    for (uint32_t i = 0x1000; i < UPDATE_EXT_IMAGE_OFFSET; ++i) { assert(external[i] == 0xA5); }
    for (uint32_t i = UPDATE_EXT_IMAGE_OFFSET + UPDATE_EXT_IMAGE_CAPACITY; i < sizeof(external); ++i) { assert(external[i] == 0xA5); }
}
static void TestTransfer(void)
{
    for (uint8_t mode = 0; mode < 2; ++mode) {
        Setup(); FwUpdate u; FwUpdate_Init(&u); assert(u.storage_ready && u.running_version == 1);
        assert(Transfer(&u, image, sizeof(image), mode));
        assert(u.state == UPDATE_PENDING && erases == 2);
        assert(memcmp(external + UPDATE_EXT_IMAGE_OFFSET, image, sizeof(image)) == 0);
        UpdateStorageRecord record; memcpy(&record, external, sizeof(record));
        assert(record.apply_mode == mode && record.image_offset == UPDATE_EXT_IMAGE_OFFSET);
        assert(record.image.commit == BOOT_IMAGE_COMMITTED && record.storage_magic == UPDATE_EXT_STORAGE_MAGIC);
        assert(external[UPDATE_EXT_IMAGE_OFFSET + sizeof(image)] == 0xFF);
        for (uint32_t i = UPDATE_EXT_IMAGE_OFFSET + W25Q16_SECTOR_SIZE;
             i < UPDATE_EXT_IMAGE_OFFSET + UPDATE_EXT_IMAGE_CAPACITY; ++i) { assert(external[i] == 0xA5); }
        FwUpdate recovered; FwUpdate_Init(&recovered);
        assert(recovered.state == UPDATE_PENDING && recovered.received == sizeof(image));
        Unchanged(); /* Existing Bootloader intentionally does not install external data. */
        const FwUpdateReply r = Request(&recovered, UPDATE_ABORT, 100, NULL, 0);
        assert(r.bytes[1] == UPDATE_OK);
        FwUpdate_Init(&recovered); assert(recovered.state == UPDATE_IDLE);
    }
    puts("PASS: real external backend, persisted defer/now mode, reset recovery, abort; old Bootloader unchanged");
}
static void TestRetriesAndErrors(void)
{
    Setup(); FwUpdate u; FwUpdate_Init(&u);
    FwUpdateReply r = Begin(&u, 1, image, sizeof(image)); assert(r.bytes[1] == UPDATE_OK);
    const unsigned before = erases;
    r = Begin(&u, 1, image, sizeof(image)); assert(r.bytes[1] == UPDATE_OK && erases == before);
    uint8_t data[36]; Update_Put32(data, 4); memcpy(data + 4, image, 32);
    r = Request(&u, UPDATE_DATA, 2, data, sizeof(data)); assert(r.bytes[1] == UPDATE_BAD_OFFSET);
    Update_Put32(data, 0);
    r = Request(&u, UPDATE_DATA, 3, data, sizeof(data)); assert(r.bytes[1] == UPDATE_OK);
    const unsigned written = writes;
    r = Request(&u, UPDATE_DATA, 4, data, sizeof(data)); assert(r.bytes[1] == UPDATE_OK && writes == written);
    data[10] ^= 1;
    r = Request(&u, UPDATE_DATA, 4, data, sizeof(data)); assert(r.bytes[1] == UPDATE_BAD_SEQUENCE);
    r = Request(&u, UPDATE_DATA, 5, data, sizeof(data)); assert(r.bytes[1] == UPDATE_BAD_OFFSET);
    uint8_t mode = 0;
    r = Request(&u, UPDATE_END, 6, &mode, 1); assert(r.bytes[1] == UPDATE_BAD_STATE);
    FwUpdate_Expire(&u); assert(u.state == UPDATE_IDLE);
    r = Request(&u, UPDATE_REBOOT, 7, NULL, 0); assert(r.bytes[1] == UPDATE_BAD_STATE);
    assert(!FwPort_Begin(0) && !FwPort_Begin(7) && !FwPort_Begin(BOOT_IMAGE_CAPACITY + 1));
    assert(!FwPort_Write(BOOT_IMAGE_CAPACITY, data, 1));
    assert(!FwPort_Read(UINT32_MAX, data, 1));
    Setup(); unavailable = true; FwUpdate_Init(&u); assert(!u.storage_ready);
    r = Request(&u, UPDATE_QUERY, 1, NULL, 0); assert(r.bytes[1] == UPDATE_FLASH_ERROR);
    unavailable = false;
    r = Request(&u, UPDATE_QUERY, 2, NULL, 0); assert(r.bytes[1] == UPDATE_OK && u.storage_ready);
    fail_read = true;
    r = Begin(&u, 3, image, sizeof(image)); assert(r.bytes[1] == UPDATE_OK);
    Update_Put32(data, 0); memcpy(data + 4, image, 32);
    r = Request(&u, UPDATE_DATA, 4, data, sizeof(data)); assert(r.bytes[1] == UPDATE_FLASH_ERROR);
    Setup(); FwUpdate_Init(&u);
    r = Begin(&u, 1, image, sizeof(image)); assert(r.bytes[1] == UPDATE_OK);
    uint8_t complete[4 + sizeof(image)]; Update_Put32(complete, 0); memcpy(complete + 4, image, sizeof(image));
    r = Request(&u, UPDATE_DATA, 2, complete, sizeof(complete)); assert(r.bytes[1] == UPDATE_OK);
    external[UPDATE_EXT_IMAGE_OFFSET + 20] ^= 1;
    r = Request(&u, UPDATE_END, 3, &mode, 1);
    assert(r.bytes[1] == UPDATE_BAD_CRC && Update_Get32(external + 24) == UINT32_MAX);
    Setup(); FwUpdate_Init(&u); Update_Put32(image + 4, 0x08000001);
    assert(!Transfer(&u, image, sizeof(image), 0));
    assert(Update_Get32(external + 24) == UINT32_MAX);
    puts("PASS: retries, sequence conflicts, range guards, incomplete image, timeout, missing chip recovery and read errors");
}
static void TestFailures(void)
{
    Setup(); FwUpdate u; FwUpdate_Init(&u); assert(Transfer(&u, image, sizeof(image), 0));
    const unsigned operations = operation;
    for (unsigned cut = 1; cut <= operations; ++cut) {
        Setup(); FwUpdate_Init(&u); fail_at = cut;
        assert(!Transfer(&u, image, sizeof(image), 0));
        fail_at = 0; FwUpdate recovered; FwUpdate_Init(&recovered);
        assert(recovered.state != UPDATE_PENDING);
        Unchanged();
    }
    Setup(); FwUpdate_Init(&u); assert(Transfer(&u, image, sizeof(image), 0));
    external[48] ^= 1; FwUpdate_Init(&u); assert(u.state == UPDATE_IDLE); /* mode CRC */
    Setup(); FwUpdate_Init(&u); assert(Transfer(&u, image, sizeof(image), 0));
    external[UPDATE_EXT_IMAGE_OFFSET + 20] ^= 1; FwUpdate_Init(&u); assert(u.state == UPDATE_IDLE);
    printf("PASS: all %u partial erase/program failures stay uncommitted; metadata/data corruption rejected\n", operations);
}
static void TestSizes(void)
{
    const uint32_t sizes[] = {8, 4095, 4096, 4097, 131072, 131073, 262144, 262145, BOOT_IMAGE_CAPACITY};
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        Setup(); FwUpdate u; FwUpdate_Init(&u);
        uint8_t begin[12] = {0}; Update_Put32(begin, sizes[i]);
        const FwUpdateReply r = Request(&u, UPDATE_BEGIN, 1, begin, sizeof(begin));
        assert(r.bytes[1] == UPDATE_OK && erases == 1U + (sizes[i] + 4095U) / 4096U);
        const uint32_t end = UPDATE_EXT_IMAGE_OFFSET + ((sizes[i] + 4095U) / 4096U) * 4096U;
        for (uint32_t j = end; j < UPDATE_EXT_IMAGE_OFFSET + BOOT_IMAGE_CAPACITY; ++j) { assert(external[j] == 0xA5); }
    }
    static uint8_t maximum[BOOT_IMAGE_CAPACITY];
    Setup(); for (uint32_t i = 0; i < sizeof(maximum); ++i) { maximum[i] = (uint8_t)i; }
    memcpy(maximum, image, 8); FwUpdate u; FwUpdate_Init(&u);
    assert(Transfer(&u, maximum, sizeof(maximum), 0));
    FwUpdate_Init(&u); assert(u.state == UPDATE_PENDING);
    assert(Transfer(&u, image, sizeof(image), 0));
    assert(memcmp(external + UPDATE_EXT_IMAGE_OFFSET + 4096, maximum + 4096, sizeof(maximum) - 4096) == 0);
    FwUpdate_Init(&u); assert(u.state == UPDATE_PENDING && u.image.image_size == sizeof(image));
    Unchanged();
    puts("PASS: 4 KiB boundaries, full 384 KiB stream CRC, large-to-small replacement ignores untouched old data");
}
static void TestParser(void)
{
    assert(Update_Crc32("123456789", 9) == 0xCBF43926);
    uint8_t wire[UPDATE_MAX_FRAME], payload[UPDATE_MAX_PAYLOAD]; memset(payload, 0x5A, sizeof(payload));
    const size_t size = UpdateProtocol_Encode(wire, sizeof(wire), UPDATE_DATA, 42, payload, sizeof(payload));
    UpdateDecoder decoder; UpdateDecoder_Reset(&decoder); UpdateFrame frame; unsigned complete = 0;
    for (size_t i = 0; i < size; ++i) {
        if (UpdateDecoder_Push(&decoder, wire[i], &frame) == UPDATE_FRAME_READY) { ++complete; }
    }
    assert(complete == 1 && frame.sequence == 42 && memcmp(frame.payload, payload, sizeof(payload)) == 0);
    wire[size - 1] ^= 1; UpdateDecodeResult result = UPDATE_INCOMPLETE;
    for (size_t i = 0; i < size; ++i) { result = UpdateDecoder_Push(&decoder, wire[i], &frame); }
    assert(result == UPDATE_FRAME_REJECTED);
    puts("PASS: byte-fragmented maximum frame and CRC rejection");
}
int main(void) { TestParser(); TestTransfer(); TestRetriesAndErrors(); TestFailures(); TestSizes(); return 0; }
