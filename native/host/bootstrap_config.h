#pragma once
// Passed by the injector's manual-map mode as DllMain's lpReserved. Derived from
// Better-Endfield's BE_BootstrapConfigV1.
#include <stdint.h>
#include <wchar.h>

#define EFML_BOOTSTRAP_MAGIC "EFMLHOST1"
#define EFML_BOOTSTRAP_PATH_CAPACITY 1024u

typedef struct EFML_BootstrapConfigV1 {
    char magic[10];
    uint32_t size;
    wchar_t data_root[EFML_BOOTSTRAP_PATH_CAPACITY];
} EFML_BootstrapConfigV1;
