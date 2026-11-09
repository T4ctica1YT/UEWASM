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
    return 0;
}
