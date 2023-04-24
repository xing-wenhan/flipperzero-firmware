#pragma once

#include "nfc_device_data.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NfcDev NfcDev;

NfcDev* nfc_dev_alloc();

void nfc_dev_free(NfcDev* instance);

bool nfc_dev_save(NfcDev* instance, NfcDevData* data, const char* path);

bool nfc_dev_load(NfcDev* instance, NfcDevData* data, const char* path);

#ifdef __cplusplus
}
#endif