#include <stdio.h>
#include <string.h>
#include <windows.h>

static unsigned fnv(const unsigned char* p, size_t n, unsigned h)
{
    while (n--) h = (h ^ *p++) * 16777619u;
    return h;
}

/* a thread spinning with pause and no system calls while another reads the game files: in the browser that read waits
   for a download, which lands only if the spin lets the page's event loop run */
static volatile LONG spin_done;
static unsigned char spin_byte;
static DWORD WINAPI spin_reader(void* arg)
{
    (void)arg;
    HANDLE h = CreateFileW(L"Content\\big.bin", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    LARGE_INTEGER o = { .QuadPart = 2900000 };
    DWORD n = 0;
    SetFilePointerEx(h, o, NULL, FILE_BEGIN);
    ReadFile(h, &spin_byte, 1, &n, NULL);
    CloseHandle(h);
    spin_done = 1;
    return 0;
}

int main(void)
{
    static unsigned char buf[3 << 20];
    char prev[64] = "none";
    HANDLE st = CreateThread(NULL, 0, spin_reader, NULL, 0, NULL);
    while (!spin_done) YieldProcessor();
    WaitForSingleObject(st, INFINITE);
    CloseHandle(st);
    printf("spin wait read %02x\n", spin_byte);
    FILE* pf = fopen("Saved\\slot.sav", "rb");
    if (pf) { prev[fread(prev, 1, sizeof prev - 1, pf)] = 0; fclose(pf); }
    printf("previous save: %s\n", prev);
    HANDLE h = CreateFileW(L"Content\\big.bin", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    LARGE_INTEGER size;
    GetFileSizeEx(h, &size);
    printf("big.bin size %lld\n", size.QuadPart);
    DWORD n, total = 0;
    unsigned sum = 2166136261u;
    while (ReadFile(h, buf, 77777, &n, NULL) && n) { sum = fnv(buf, n, sum); total += n; }
    printf("sequential %lu bytes fnv %08x\n", total, sum);
    const long long offs[] = { 0, 1048575, 1048576, 2097000, 3000000, 12345 };
    for (int i = 0; i < 6; i++) {
        LARGE_INTEGER o = { .QuadPart = offs[i] };
        SetFilePointerEx(h, o, NULL, FILE_BEGIN);
        ReadFile(h, buf, 200000, &n, NULL);
        printf("at %lld got %lu fnv %08x\n", offs[i], n, fnv(buf, n, 2166136261u));
    }
    CloseHandle(h);

    FILE* f = fopen("Content\\words.txt", "rb");
    if (f) {
        size_t got = fread(buf, 1, sizeof buf, f);
        printf("words %zu bytes fnv %08x\n", got, fnv(buf, got, 2166136261u));
        fseek(f, 1500000, SEEK_SET);
        got = fread(buf, 1, 900000, f);
        printf("words at 1500000 got %zu fnv %08x\n", got, fnv(buf, got, 2166136261u));
        fclose(f);
    }

    f = fopen("Content\\Sub\\small.txt", "rb");
    char line[128] = { 0 };
    if (f) { fgets(line, sizeof line, f); fclose(f); }
    printf("small: %s\n", f ? line : "(missing)");

    WIN32_FIND_DATAW fd;
    HANDLE fh = FindFirstFileW(L"Content\\*", &fd);
    int files = 0, dirs = 0;
    if (fh != INVALID_HANDLE_VALUE) {
        do {
            if (fd.cFileName[0] == '.') continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) dirs++; else files++;
        } while (FindNextFileW(fh, &fd));
        FindClose(fh);
    }
    printf("Content has %d files, %d dirs\n", files, dirs);
    printf("case-insensitive: %d\n", GetFileAttributesW(L"CONTENT\\SUB\\SMALL.TXT") != INVALID_FILE_ATTRIBUTES);
    printf("stub dll listed: %d\n", GetFileAttributesW(L"Binaries\\helper.dll") != INVALID_FILE_ATTRIBUTES);

    CreateDirectoryW(L"Saved", NULL);
    h = CreateFileW(L"Saved\\slot.sav", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    WriteFile(h, "progress=7", 10, &n, NULL);
    CloseHandle(h);
    f = fopen("Saved\\slot.sav", "rb");
    memset(line, 0, sizeof line);
    if (f) { fread(line, 1, sizeof line - 1, f); fclose(f); }
    printf("save: %s\n", line);
    DWORD missing = GetFileAttributesW(L"Content\\nope.bin");
    printf("missing invalid %d err %lu\n", missing == INVALID_FILE_ATTRIBUTES, GetLastError());
    printf("done\n");
    return 0;
}
