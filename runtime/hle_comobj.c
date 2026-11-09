/* COM classes that engines create without checking the result. WMI connects and returns no rows;
   the WIC factory exists but every decode fails (splash images are skipped). */
#include "com.h"

static const uint8_t CLSID_WICImagingFactory[16] = { 0x62, 0xF2, 0xCA, 0xCA, 0x70, 0x93, 0x15, 0x46, 0xA1, 0x3B, 0x9F, 0x55, 0x39, 0xDA, 0x4C, 0x0A };
static const uint8_t CLSID_WbemLocator[16] = { 0x11, 0xF8, 0x90, 0x45, 0x3A, 0x1D, 0xD0, 0x11, 0x89, 0x1F, 0x00, 0xAA, 0x00, 0x4B, 0x2E, 0x24 };

static ComKind K_Locator = { "IWbemLocator", 4 };
static ComKind K_Services = { "IWbemServices", 23 };
static ComKind K_Enum = { "IEnumWbemClassObject", 8 };
static ComKind K_WIC = { "IWICImagingFactory", 28 };

static void ok(CPU* c) { RET(0); }
static void wic_fail(CPU* c) { RET(0x88982F50u); }
static void connect_server(CPU* c) { if (ARG(9)) ST64(ARG(9), com_new(&K_Services, NULL, 0)); RET(0); }
static void exec_query(CPU* c) { if (ARG(6)) ST64(ARG(6), com_new(&K_Enum, NULL, 0)); RET(0); }
static void enum_next(CPU* c) { if (ARG(5)) ST32(ARG(5), 0); RET(1); }

static void setup(void)
{
    static int done;
    if (done) return;
    done = 1;
    com_iunknown(&K_Locator);
    com_iunknown(&K_Services);
    com_iunknown(&K_Enum);
    K_Locator.slots[3] = connect_server;
    for (int i = 3; i < 23; i++) K_Services.slots[i] = ok;
    K_Services.slots[18] = exec_query;
    K_Services.slots[20] = exec_query;
    K_Enum.slots[3] = ok;
    K_Enum.slots[4] = enum_next;
    K_Enum.slots[7] = ok;
    com_iunknown(&K_WIC);
    for (int i = 3; i < 28; i++) K_WIC.slots[i] = wic_fail;
}

/* returns 1 if handled */
int xaudio_create(CPU* c, uint64_t clsid, uint64_t out);

int com_create_known(CPU* c, uint64_t clsid, uint64_t out)
{
    if (xaudio_create(c, clsid, out)) return 1;
    ComKind* k = iid_eq(clsid, CLSID_WbemLocator) ? &K_Locator : iid_eq(clsid, CLSID_WICImagingFactory) ? &K_WIC : NULL;
    if (!k) return 0;
    setup();
    if (out) ST64(out, com_new(k, NULL, 0));
    RET(0);
    return 1;
}
