#include <stdio.h>
#include <windows.h>

static CRITICAL_SECTION cs;
static volatile LONG counter;
static long long totals[4];
static HANDLE ready_evt, go_evt;

static DWORD WINAPI worker(LPVOID p)
{
    int id = (int)(INT_PTR)p;
    SetEvent(ready_evt);
    WaitForSingleObject(go_evt, INFINITE);
    for (int i = 0; i < 20000; i++) {
        InterlockedIncrement(&counter);
        EnterCriticalSection(&cs);
        totals[id] += i % (id + 3);
        LeaveCriticalSection(&cs);
        if (i % 1000 == 0) Sleep(0);
    }
    return 100 + id;
}

int main(void)
{
    InitializeCriticalSection(&cs);
    ready_evt = CreateEventW(NULL, FALSE, FALSE, NULL);
    go_evt = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE th[4];
    for (int i = 0; i < 4; i++) {
        th[i] = CreateThread(NULL, 0, worker, (LPVOID)(INT_PTR)i, 0, NULL);
        WaitForSingleObject(ready_evt, INFINITE);
    }
    SetEvent(go_evt);
    WaitForMultipleObjects(4, th, TRUE, INFINITE);
    for (int i = 0; i < 4; i++) {
        DWORD code = 0;
        GetExitCodeThread(th[i], &code);
        printf("thread %d exit %lu total %lld\n", i, code, totals[i]);
        CloseHandle(th[i]);
    }
    printf("counter %ld\n", counter);
    return 0;
}
