#include "platform_fs.h"

#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <wchar.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

static int failures;
#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", \
                __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

int main(void) {
    static const uint8_t payload[] = { 0, 1, 2, 3, 0x7f, 0x80, 0xff };
    const char* path = "threedcad-\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88-atomic.bin";
    uint8_t* loaded = NULL;
    size_t size = 0;
    CadResult result;

    result = CadPlatform_WriteFileAtomic(path, payload, sizeof(payload));
    CHECK(CadResult_IsSuccess(&result));
    result = CadPlatform_ReadFile(path, sizeof(payload), &loaded, &size);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(size == sizeof(payload));
    CHECK(loaded != NULL && memcmp(loaded, payload, sizeof(payload)) == 0);
    CHECK(CadPlatform_PathsEqual(path, path));
#ifdef _WIN32
    CHECK(CadPlatform_PathsEqual(
        path, ".\\threedcad-\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88-atomic.bin"));
#else
    CHECK(CadPlatform_PathsEqual(
        path, "./threedcad-\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88-atomic.bin"));
#endif
    CadPlatform_Free(loaded);
    loaded = NULL;

    result = CadPlatform_ReadFile(path, sizeof(payload) - 1, &loaded, &size);
    CHECK(!CadResult_IsSuccess(&result));
    CHECK(loaded == NULL);

    result = CadPlatform_WriteFileAtomic(
        "missing-platform-test-directory/output.bin", payload,
        sizeof(payload));
    CHECK(!CadResult_IsSuccess(&result));

    /* A replacement failure must leave an existing destination byte exact.
       Windows sharing denial deterministically reaches the post-flush atomic
       replacement path rather than failing before the temporary write. */
#ifdef _WIN32
    {
        static const uint8_t original[] = {9, 8, 7, 6};
        static const char failureDirectory[] = "threedcad-atomic-failure";
        static const char failurePath[] =
            "threedcad-atomic-failure/existing.bin";
        HANDLE lock;
        CreateDirectoryA(failureDirectory, NULL);
        result = CadPlatform_WriteFileAtomic(failurePath, original,
                                             sizeof(original));
        CHECK(CadResult_IsSuccess(&result));
        lock = CreateFileA(failurePath, GENERIC_READ, 0, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
        CHECK(lock != INVALID_HANDLE_VALUE);
        result = CadPlatform_WriteFileAtomic(failurePath, payload,
                                             sizeof(payload));
        CHECK(!CadResult_IsSuccess(&result));
        if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
        result = CadPlatform_ReadFile(failurePath, sizeof(payload),
                                      &loaded, &size);
        CHECK(CadResult_IsSuccess(&result));
        CHECK(size == sizeof(original));
        CHECK(loaded && memcmp(loaded, original, sizeof(original)) == 0);
        CadPlatform_Free(loaded);
        loaded = NULL;
        DeleteFileA(failurePath);
        RemoveDirectoryA(failureDirectory);
    }
#endif

#ifdef _WIN32
    _wremove(L"threedcad-\x30c6\x30b9\x30c8-atomic.bin");
#else
    remove(path);
#endif
    if (failures) {
        fprintf(stderr, "%d platform filesystem test(s) failed\n", failures);
        return 1;
    }
    puts("All platform filesystem tests passed.");
    return 0;
}
