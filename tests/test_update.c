/* Native tests use the actual protocol/service and Bootloader core. */
#include "fw_update.h"
#include "fw_update_port.h"
#include "bootloader.h"
#include "boot_port.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t flash[BOOT_FLASH_SIZE];
static uint8_t image[67];
static unsigned operation, fail_at, erases, writes;
static uint32_t crc_state;
static bool unlocked;

static uint8_t *At(uint32_t address) { assert(address >= BOOT_FLASH_BASE && address < BOOT_FLASH_BASE + BOOT_FLASH_SIZE); return flash + address - BOOT_FLASH_BASE; }
static bool Step(void) { return ++operation != fail_at; }
static bool EraseAt(uint32_t address, uint32_t size)
{
    if (!Step()) { return false; }
    memset(At(address), 0xFF, size);
    ++erases;
    return true;
}
static bool Program(uint32_t address, uint32_t value)
{
    if (!Step()) { return false; }
    uint32_t old;
    memcpy(&old, At(address), 4);
    assert((old & value) == value);
    memcpy(At(address), &value, 4);
    ++writes;
    return true;
}

bool FwPort_Begin(void)
{
    return EraseAt(BOOT_REQUEST_ADDRESS, 0x4000) &&
           EraseAt(BOOT_STAGE_ADDRESS, 0x20000) &&
           EraseAt(BOOT_STAGE_ADDRESS + 0x20000, 0x20000) &&
           EraseAt(BOOT_STAGE_ADDRESS + 0x40000, 0x20000);
}
bool FwPort_Cancel(void) { return EraseAt(BOOT_REQUEST_ADDRESS, 0x4000); }
bool FwPort_Write(uint32_t offset, const uint8_t *data, uint32_t size)
{
    assert(offset + size <= BOOT_IMAGE_CAPACITY);
    for (uint32_t i = 0; i < size; i += 4) {
        uint32_t word = UINT32_MAX;
        memcpy(&word, data + i, size - i < 4 ? size - i : 4);
        if (!Program(BOOT_STAGE_ADDRESS + offset + i, word)) { return false; }
    }
    return true;
}
const uint8_t *FwPort_Stage(uint32_t offset, uint32_t size)
{
    assert(offset <= BOOT_IMAGE_CAPACITY && size <= BOOT_IMAGE_CAPACITY - offset);
    return At(BOOT_STAGE_ADDRESS + offset);
}
void FwPort_ReadRequest(BootImageManifest *m) { memcpy(m, At(BOOT_REQUEST_ADDRESS), sizeof(*m)); }
void FwPort_ReadInstalled(BootImageManifest *m) { memcpy(m, At(BOOT_INSTALLED_ADDRESS), sizeof(*m)); }
bool FwPort_Commit(const BootImageManifest *m)
{
    for (uint32_t i = 0; i <= 24; i += 4) {
        uint32_t word;
        memcpy(&word, (const uint8_t *)m + i, 4);
        if (i < 24) { assert(Update_Get32(At(BOOT_REQUEST_ADDRESS + 24)) == UINT32_MAX); }
        if (!Program(BOOT_REQUEST_ADDRESS + i, word)) { return false; }
    }
    return true;
}

/* Bootloader's port uses the same NOR model and a byte-stream CRC adapter. */
const void *BootPort_Map(uint32_t address) { return At(address); }
void BootPort_CrcBegin(void) { crc_state = UINT32_MAX; }
void BootPort_CrcFeedWord(uint32_t word) { crc_state = Update_CrcContinue(crc_state, &word, 4); }
uint32_t BootPort_CrcFinish(void) { return crc_state; }
bool BootPort_Unlock(void) { unlocked = true; return true; }
void BootPort_Lock(void) { unlocked = false; }
bool BootPort_EraseInstalled(void) { assert(unlocked); return EraseAt(BOOT_INSTALLED_ADDRESS, 0x4000); }
bool BootPort_EraseApplication(void) { assert(unlocked); return EraseAt(BOOT_APP_ADDRESS, BOOT_IMAGE_CAPACITY); }
bool BootPort_ProgramWord(uint32_t address, uint32_t value)
{
    assert(unlocked);
    assert((address >= BOOT_APP_ADDRESS && address < BOOT_APP_ADDRESS + BOOT_IMAGE_CAPACITY) ||
           (address >= BOOT_INSTALLED_ADDRESS && address <= BOOT_INSTALLED_ADDRESS + 24) ||
           (address == BOOT_REQUEST_ADDRESS + 28 && value == 0));
    return Program(address, value);
}

static BootImageManifest Manifest(uint32_t version)
{
    BootImageManifest m = {BOOT_IMAGE_MAGIC, BOOT_IMAGE_FORMAT, sizeof(image),
        0, version, 0, BOOT_IMAGE_COMMITTED, BOOT_ERASED_WORD};
    m.image_crc = Update_Crc32(image, sizeof(image));
    m.header_crc = Update_Crc32(&m, 20);
    return m;
}

static void Setup(void)
{
    memset(flash, 0xA5, sizeof(flash));
    memset(At(BOOT_REQUEST_ADDRESS), 0xFF, 0x4000);
    for (unsigned i = 0; i < sizeof(image); ++i) { image[i] = (uint8_t)(i * 7); }
    Update_Put32(image, 0x20020000);
    Update_Put32(image + 4, BOOT_APP_ADDRESS + 9);
    memcpy(At(BOOT_APP_ADDRESS), image, sizeof(image));
    const BootImageManifest installed = Manifest(1);
    memcpy(At(BOOT_INSTALLED_ADDRESS), &installed, sizeof(installed));
    operation = fail_at = erases = writes = 0;
    unlocked = false;
}

static FwUpdateReply Request(FwUpdate *u, uint8_t command, uint32_t sequence, const void *data, uint16_t length)
{
    const UpdateFrame frame = {command, sequence, length, data};
    FwUpdateReply reply;
    FwUpdate_Process(u, &frame, &reply);
    return reply;
}

static bool Transfer(FwUpdate *u, uint8_t mode)
{
    uint8_t begin[12], data[UPDATE_MAX_PAYLOAD];
    Update_Put32(begin, sizeof(image));
    Update_Put32(begin + 4, Update_Crc32(image, sizeof(image)));
    Update_Put32(begin + 8, 2);
    FwUpdateReply reply = Request(u, UPDATE_BEGIN, 10, begin, sizeof(begin));
    if (reply.bytes[1] != UPDATE_OK) { return false; }
    uint32_t sequence = 11;
    for (uint32_t offset = 0; offset < sizeof(image); offset += 32) {
        const uint32_t count = sizeof(image) - offset < 32 ? sizeof(image) - offset : 32;
        Update_Put32(data, offset);
        memcpy(data + 4, image + offset, count);
        reply = Request(u, UPDATE_DATA, sequence++, data, (uint16_t)(count + 4));
        if (reply.bytes[1] != UPDATE_OK) { return false; }
    }
    reply = Request(u, UPDATE_END, sequence, &mode, 1);
    if (reply.bytes[1] != UPDATE_OK) { return false; }
    assert(reply.reset_after_ack == (mode == 1));
    return true;
}

static void TestTransferAndBoot(void)
{
    for (uint8_t mode = 0; mode < 2; ++mode) {
        Setup();
        FwUpdate u;
        FwUpdate_Init(&u);
        assert(Transfer(&u, mode));
        assert(Update_Get32(At(BOOT_INSTALLED_ADDRESS + 16)) == 1); /* not installed before reset */
        assert(Update_Get32(At(BOOT_REQUEST_ADDRESS + 28)) == UINT32_MAX);
        assert(memcmp(At(BOOT_STAGE_ADDRESS), image, sizeof(image)) == 0);
        assert(At(BOOT_STAGE_ADDRESS)[sizeof(image)] == 0xFF); /* padding excluded from CRC */
        FwUpdate recovered;
        FwUpdate_Init(&recovered);
        assert(recovered.state == UPDATE_PENDING);
        assert(Boot_PrepareApplication() == BOOT_READY);
        assert(Update_Get32(At(BOOT_INSTALLED_ADDRESS + 16)) == 2);
        assert(Update_Get32(At(BOOT_REQUEST_ADDRESS + 28)) == 0);
        const unsigned before = erases;
        assert(Boot_PrepareApplication() == BOOT_READY && erases == before);
        for (uint32_t i = 0; i < BOOT_LOADER_SIZE; ++i) { assert(flash[i] == 0xA5); }
        assert(*At(0x08004000) == 0xA5 && *At(0x08010000) == 0xA5 && *At(0x080E0000) == 0xA5);
    }
    puts("PASS: defer/immediate requests install with the actual Bootloader, no repeat installation");
}

static void TestRetriesAndValidation(void)
{
    Setup();
    FwUpdate u;
    FwUpdate_Init(&u);
    uint8_t begin[12], data[36];
    Update_Put32(begin, sizeof(image)); Update_Put32(begin + 4, Update_Crc32(image, sizeof(image))); Update_Put32(begin + 8, 3);
    FwUpdateReply r = Request(&u, UPDATE_BEGIN, 1, begin, 12);
    assert(r.bytes[1] == UPDATE_OK);
    const unsigned erased = erases;
    r = Request(&u, UPDATE_BEGIN, 1, begin, 12);
    assert(r.bytes[1] == UPDATE_OK && erases == erased);
    begin[8] ^= 1;
    r = Request(&u, UPDATE_BEGIN, 1, begin, 12);
    assert(r.bytes[1] == UPDATE_BAD_SEQUENCE && erases == erased);
    Update_Put32(data, 4); memcpy(data + 4, image, 32);
    r = Request(&u, UPDATE_DATA, 2, data, sizeof(data));
    assert(r.bytes[1] == UPDATE_BAD_OFFSET && u.received == 0);
    Update_Put32(data, 0);
    r = Request(&u, UPDATE_DATA, 3, data, sizeof(data));
    assert(r.bytes[1] == UPDATE_OK && u.received == 32);
    const unsigned written = writes;
    r = Request(&u, UPDATE_DATA, 4, data, sizeof(data));
    assert(r.bytes[1] == UPDATE_OK && u.received == 32 && writes == written);
    data[10] ^= 1;
    r = Request(&u, UPDATE_DATA, 5, data, sizeof(data));
    assert(r.bytes[1] == UPDATE_BAD_OFFSET);
    uint8_t mode = 0;
    r = Request(&u, UPDATE_END, 6, &mode, 1);
    assert(r.bytes[1] == UPDATE_BAD_STATE);
    FwUpdate_Expire(&u);
    assert(u.state == UPDATE_IDLE);
    r = Request(&u, UPDATE_REBOOT, 7, NULL, 0);
    assert(r.bytes[1] == UPDATE_BAD_STATE && !r.reset_after_ack);
    assert(Boot_PrepareApplication() == BOOT_READY);

    Setup(); FwUpdate_Init(&u); assert(Transfer(&u, 0));
    r = Request(&u, UPDATE_ABORT, 100, NULL, 0);
    assert(r.bytes[1] == UPDATE_OK && u.state == UPDATE_IDLE);
    assert(Boot_PrepareApplication() == BOOT_READY);
    assert(Update_Get32(At(BOOT_INSTALLED_ADDRESS + 16)) == 1);

    Setup(); FwUpdate_Init(&u);
    image[4] = 0; image[5] = 0; image[6] = 0; image[7] = 8;
    assert(!Transfer(&u, 0));
    assert(Update_Get32(At(BOOT_REQUEST_ADDRESS + 24)) == UINT32_MAX);
    puts("PASS: retries, conflicting sequence, offsets, incomplete image, timeout, cancellation, invalid vectors");
}

static void TestPowerFailures(void)
{
    Setup(); FwUpdate u; FwUpdate_Init(&u); assert(Transfer(&u, 0));
    const unsigned operations = operation;
    for (unsigned cut = 1; cut <= operations; ++cut) {
        Setup(); FwUpdate_Init(&u); fail_at = cut;
        assert(!Transfer(&u, 0));
        fail_at = 0;
        assert(Boot_PrepareApplication() == BOOT_READY);
        assert(Update_Get32(At(BOOT_INSTALLED_ADDRESS + 16)) == 1);
    }
    printf("PASS: old APP preserved at all %u receive-side erase/program failure points\n", operations);
}

static void TestMaximumAndReadbackCrc(void)
{
    static uint8_t maximum[BOOT_IMAGE_CAPACITY];
    Setup();
    for (uint32_t i = 0; i < sizeof(maximum); ++i) { maximum[i] = (uint8_t)i; }
    memcpy(maximum, image, 8);
    FwUpdate u; FwUpdate_Init(&u);
    uint8_t begin[12], data[UPDATE_MAX_PAYLOAD];
    Update_Put32(begin, sizeof(maximum));
    Update_Put32(begin + 4, Update_Crc32(maximum, sizeof(maximum)));
    Update_Put32(begin + 8, 4);
    FwUpdateReply r = Request(&u, UPDATE_BEGIN, 1, begin, 12);
    assert(r.bytes[1] == UPDATE_OK);
    uint32_t sequence = 2;
    for (uint32_t offset = 0; offset < sizeof(maximum); offset += UPDATE_CHUNK_SIZE) {
        Update_Put32(data, offset);
        memcpy(data + 4, maximum + offset, UPDATE_CHUNK_SIZE);
        r = Request(&u, UPDATE_DATA, sequence++, data, sizeof(data));
        assert(r.bytes[1] == UPDATE_OK && u.received == offset + UPDATE_CHUNK_SIZE);
    }
    uint8_t mode = 0;
    At(BOOT_STAGE_ADDRESS)[100] ^= 1;
    r = Request(&u, UPDATE_END, sequence++, &mode, 1);
    assert(r.bytes[1] == UPDATE_BAD_CRC);
    assert(Update_Get32(At(BOOT_REQUEST_ADDRESS + 24)) == UINT32_MAX);
    At(BOOT_STAGE_ADDRESS)[100] ^= 1;
    r = Request(&u, UPDATE_END, sequence, &mode, 1);
    assert(r.bytes[1] == UPDATE_OK);
    assert(Boot_PrepareApplication() == BOOT_READY);
    assert(memcmp(At(BOOT_APP_ADDRESS), maximum, sizeof(maximum)) == 0);
    assert(*At(0x080E0000) == 0xA5);
    puts("PASS: full 384 KiB transfer/install, exact partition boundary and readback CRC rejection");
}

static void TestParser(void)
{
    assert(Update_Crc32("123456789", 9) == 0xCBF43926);
    uint8_t wire[UPDATE_MAX_FRAME];
    uint8_t payload[UPDATE_MAX_PAYLOAD]; memset(payload, 0x5A, sizeof(payload));
    const size_t size = UpdateProtocol_Encode(wire, sizeof(wire), UPDATE_DATA, 42, payload, sizeof(payload));
    assert(size == sizeof(wire));
    UpdateDecoder d; UpdateDecoder_Reset(&d); UpdateFrame f;
    const uint8_t noise[] = {'F', 'F', 'X', 0x55, 'F', 'W'};
    for (unsigned i = 0; i < sizeof(noise); ++i) { (void)UpdateDecoder_Push(&d, noise[i], &f); }
    unsigned complete = 0;
    for (size_t i = 0; i < size; ++i) {
        if (UpdateDecoder_Push(&d, wire[i], &f) == UPDATE_FRAME_READY) { ++complete; }
    }
    assert(complete == 1 && f.sequence == 42 && f.length == sizeof(payload));
    assert(memcmp(f.payload, payload, sizeof(payload)) == 0);
    wire[size - 1] ^= 1;
    UpdateDecodeResult result = UPDATE_INCOMPLETE;
    for (size_t i = 0; i < size; ++i) { result = UpdateDecoder_Push(&d, wire[i], &f); }
    assert(result == UPDATE_FRAME_REJECTED);
    wire[6] = 0xFF; wire[7] = 0xFF;
    for (unsigned i = 0; i < 16; ++i) { result = UpdateDecoder_Push(&d, wire[i], &f); }
    assert(result == UPDATE_FRAME_REJECTED && d.used == 0);
    puts("PASS: CRC-32, byte-fragmented maximum frame, noise resynchronization, bad CRC and oversized length");
}

int main(void)
{
    TestParser(); TestTransferAndBoot(); TestRetriesAndValidation(); TestPowerFailures();
    TestMaximumAndReadbackCrc();
    return 0;
}
