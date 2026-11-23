#include <stdio.h>
#include <string.h>
#include <windows.h>

int main(void)
{
    CreateDirectoryW(L"Saved", NULL);
    CreateDirectoryW(L"Saved\\Config", NULL);
    HANDLE h = CreateFileW(L"Saved\\Config\\Game.ini", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    const char* text = "[Settings]\r\nResX=1280\r\nResY=720\r\n";
    DWORD n = 0;
    WriteFile(h, text, (DWORD)strlen(text), &n, NULL);
    CloseHandle(h);
    printf("wrote %lu bytes\n", n);

    FILE* f = fopen("Saved\\Config\\Game.ini", "a");
    fprintf(f, "Fullscreen=%d\n", 1);
    fclose(f);

    char buf[256] = { 0 };
    f = fopen("Saved\\Config\\Game.ini", "rb");
    size_t got = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    printf("read %zu bytes, lines:", got);
    for (char* line = strtok(buf, "\r\n"); line; line = strtok(NULL, "\r\n")) printf(" [%s]", line);
    printf("\n");

    WIN32_FILE_ATTRIBUTE_DATA a;
    printf("attr ok %d size %lu\n", GetFileAttributesExW(L"Saved\\Config\\Game.ini", GetFileExInfoStandard, &a), a.nFileSizeLow);

    for (int i = 0; i < 3; i++) {
        wchar_t name[64];
        swprintf(name, 64, L"Saved\\slot%d.sav", i);
        h = CreateFileW(name, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        WriteFile(h, name, (DWORD)(wcslen(name) * 2), &n, NULL);
        CloseHandle(h);
    }
    WIN32_FIND_DATAW fd;
    HANDLE fh = FindFirstFileW(L"Saved\\*.sav", &fd);
    int count = 0;
    if (fh != INVALID_HANDLE_VALUE) {
        do count++; while (FindNextFileW(fh, &fd));
        FindClose(fh);
    }
    printf("found %d save files\n", count);
    DeleteFileW(L"Saved\\slot1.sav");
    printf("after delete exists: %d\n", GetFileAttributesW(L"Saved\\slot1.sav") != INVALID_FILE_ATTRIBUTES);
    DWORD missing = GetFileAttributesW(L"Nope\\nothing.txt");
    printf("missing file attr invalid: %d err %lu\n", missing == INVALID_FILE_ATTRIBUTES, GetLastError());

    // a read-write mapping bigger than the file grows it; writes through the view reach the file even after the
    // file handle is closed; a copy-on-write view stays private; ReOpenFile reads the same file
    HANDLE mf = CreateFileW(L"Saved\\mapped.bin", GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD w2;
    WriteFile(mf, "head", 4, &w2, NULL);
    HANDLE mm = CreateFileMappingW(mf, NULL, PAGE_READWRITE, 0, 8192, NULL);
    CloseHandle(mf);
    char* v = mm ? (char*)MapViewOfFile(mm, FILE_MAP_WRITE, 0, 0, 0) : NULL;
    printf("map rw: %d, starts with %.4s\n", v != NULL, v ? v : "");
    if (v) { memcpy(v + 4096, "mapped write", 12); FlushViewOfFile(v, 0); UnmapViewOfFile(v); }
    char* cv = mm ? (char*)MapViewOfFile(mm, FILE_MAP_COPY, 0, 0, 0) : NULL;
    if (cv) { memcpy(cv, "XXXX", 4); UnmapViewOfFile(cv); }
    if (mm) CloseHandle(mm);
    mf = CreateFileW(L"Saved\\mapped.bin", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    char back[16] = { 0 };
    LARGE_INTEGER sz;
    GetFileSizeEx(mf, &sz);
    ReadFile(mf, back, 4, &w2, NULL);
    SetFilePointer(mf, 4096, NULL, FILE_BEGIN);
    ReadFile(mf, back + 4, 12, &w2, NULL);
    printf("mapped file: size %lld, [%.4s] [%.12s]\n", sz.QuadPart, back, back + 4);
    HANDLE ro = ReOpenFile(mf, GENERIC_READ, FILE_SHARE_READ, 0);
    char again[5] = { 0 };
    if (ro != INVALID_HANDLE_VALUE) { ReadFile(ro, again, 4, &w2, NULL); CloseHandle(ro); }
    printf("reopened: %d [%s]\n", ro != INVALID_HANDLE_VALUE, again);
    CloseHandle(mf);
    HANDLE ef = CreateFileW(L"Saved\\empty.bin", GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    HANDLE em = CreateFileMappingW(ef, NULL, PAGE_READONLY, 0, 0, NULL);
    printf("empty file mapping: %d err %lu\n", em != NULL, GetLastError());
    if (em) CloseHandle(em);
    CloseHandle(ef);
    DeleteFileW(L"Saved\\mapped.bin");
    DeleteFileW(L"Saved\\empty.bin");
    return 0;
}
