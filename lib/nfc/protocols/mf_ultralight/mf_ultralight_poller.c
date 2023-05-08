#include "mf_ultralight_poller_i.h"

#include <furi.h>

#define TAG "MfUltralightPoller"

#define MF_ULTRALIGHT_MAX_BUFF_SIZE (64)
#define MF_ULTRALIGHT_DEFAULT_PASSWORD (0xffffffffUL)

typedef MfUltralightPollerCommand (*MfUltralightPollerReadHandler)(MfUltralightPoller* instance);

static NfcaPollerCommand mf_ultralight_process_command(MfUltralightPollerCommand command) {
    NfcaPollerCommand ret = NfcaPollerCommandContinue;

    if(command == MfUltralightPollerCommandContinue) {
        ret = NfcaPollerCommandContinue;
    } else if(command == MfUltralightPollerCommandReset) {
        ret = NfcaPollerCommandReset;
    } else if(command == MfUltralightPollerCommandStop) {
        ret = NfcaPollerCommandStop;
    } else {
        furi_crash("Unknown command");
    }

    return ret;
}

MfUltralightPoller* mf_ultralight_poller_alloc(NfcaPoller* nfca_poller) {
    MfUltralightPoller* instance = malloc(sizeof(MfUltralightPoller));
    instance->nfca_poller = nfca_poller;

    return instance;
}

void mf_ultralight_poller_free(MfUltralightPoller* instance) {
    furi_assert(instance);

    free(instance);
}

static MfUltralightPollerCommand mf_ultralight_poller_handler_idle(MfUltralightPoller* instance) {
    nfc_poller_buffer_reset(instance->buffer);
    nfca_poller_get_data(instance->nfca_poller, &instance->data->nfca_data);
    instance->counters_read = 0;
    instance->counters_total = 3;
    instance->tearing_flag_read = 0;
    instance->tearing_flag_total = 3;
    instance->pages_read = 0;
    instance->state = MfUltralightPollerStateReadVersion;

    return MfUltralightPollerCommandContinue;
}

static MfUltralightPollerCommand
    mf_ultralight_poller_handler_read_version(MfUltralightPoller* instance) {
    instance->error = mf_ultralight_poller_async_read_version(instance, &instance->data->version);
    if(instance->error == MfUltralightErrorNone) {
        FURI_LOG_D(TAG, "Read version success");
        instance->data->type = mf_ultralight_get_type_by_version(&instance->data->version);
        instance->state = MfUltralightPollerStateGetFeatureSet;
    } else {
        FURI_LOG_D(TAG, "Didn't response. Check NTAG 203");
        nfca_poller_halt(instance->nfca_poller);
        instance->state = MfUltralightPollerStateDetectNtag203;
    }

    return MfUltralightPollerCommandContinue;
}

static MfUltralightPollerCommand
    mf_ultralight_poller_handler_check_ntag_203(MfUltralightPoller* instance) {
    MfUltralightPageReadCommandData data = {};
    instance->error = mf_ultralight_poller_async_read_page(instance, 41, &data);
    if(instance->error == MfUltralightErrorNone) {
        FURI_LOG_D(TAG, "NTAG203 detected");
        instance->data->type = MfUltralightTypeNTAG203;
    } else {
        FURI_LOG_D(TAG, "Original Ultralight detected");
        nfca_poller_halt(instance->nfca_poller);
        instance->data->type = MfUltralightTypeUnknown;
    }
    instance->state = MfUltralightPollerStateGetFeatureSet;

    return MfUltralightPollerCommandContinue;
}

static MfUltralightPollerCommand
    mf_ultralight_poller_handler_get_feature_set(MfUltralightPoller* instance) {
    MfUltralightPollerCommand command = MfUltralightPollerCommandContinue;

    instance->feature_set = mf_ultralight_get_feature_support_set(instance->data->type);
    instance->pages_total = mf_ultralight_get_pages_total(instance->data->type);
    instance->data->pages_total = instance->pages_total;
    FURI_LOG_D(
        TAG,
        "%s detected. Total pages: %d",
        mf_ultralight_get_name(instance->data->type, true),
        instance->pages_total);

    instance->state = MfUltralightPollerStateReadSignature;
    return command;
}

static MfUltralightPollerCommand
    mf_ultralight_poller_handler_read_signature(MfUltralightPoller* instance) {
    MfUltralightPollerState next_state = MfUltralightPollerStateAuth;
    if(instance->feature_set & MfUltralightFeatureSupportReadSignature) {
        FURI_LOG_D(TAG, "Reading signature");
        instance->error =
            mf_ultralight_poller_async_read_signature(instance, &instance->data->signature);
        if(instance->error != MfUltralightErrorNone) {
            FURI_LOG_D(TAG, "Read signature failed");
            next_state = MfUltralightPollerStateReadFailed;
        }
    } else {
        FURI_LOG_D(TAG, "Skip reading signature");
    }
    instance->state = next_state;

    return MfUltralightPollerCommandContinue;
}

static MfUltralightPollerCommand
    mf_ultralight_poller_handler_read_counters(MfUltralightPoller* instance) {
    if(instance->feature_set & MfUltralightFeatureSupportReadCounter) {
        if(mf_ultralight_is_counter_configured(instance->data)) {
            if(instance->feature_set & MfUltralightFeatureSupportSingleCounter) {
                instance->counters_read = 2;
            }
            if(instance->counters_read == instance->counters_total) {
                instance->state = MfUltralightPollerStateReadTearingFlags;
            } else {
                FURI_LOG_D(TAG, "Reading counter %d", instance->counters_read);
                instance->error = mf_ultralight_poller_async_read_counter(
                    instance,
                    instance->counters_read,
                    &instance->data->counter[instance->counters_read]);
                if(instance->error != MfUltralightErrorNone) {
                    FURI_LOG_D(TAG, "Failed to read %d counter", instance->counters_read);
                    instance->state = MfUltralightPollerStateReadTearingFlags;
                } else {
                    instance->counters_read++;
                }
            }
        } else {
            instance->state = MfUltralightPollerStateReadTearingFlags;
        }
    } else {
        FURI_LOG_D(TAG, "Skip reading counters");
        instance->state = MfUltralightPollerStateReadTearingFlags;
    }

    return MfUltralightPollerCommandContinue;
}

static MfUltralightPollerCommand
    mf_ultralight_poller_handler_read_tearing_flags(MfUltralightPoller* instance) {
    if(instance->feature_set & MfUltralightFeatureSupportCheckTearingFlag) {
        if(instance->tearing_flag_read == instance->tearing_flag_total) {
            instance->state = MfUltralightPollerStateTryDefaultPass;
        } else {
            FURI_LOG_D(TAG, "Reading tearing flag %d", instance->tearing_flag_read);
            instance->error = mf_ultralight_poller_async_read_tearing_flag(
                instance,
                instance->tearing_flag_read,
                &instance->data->tearing_flag[instance->tearing_flag_read]);
            if(instance->error != MfUltralightErrorNone) {
                FURI_LOG_D(TAG, "Reading tearing flag %d failed", instance->tearing_flag_read);
                instance->state = MfUltralightPollerStateReadFailed;
            } else {
                instance->tearing_flag_read++;
            }
        }
    } else {
        FURI_LOG_D(TAG, "Skip reading tearing flags");
        instance->state = MfUltralightPollerStateTryDefaultPass;
    }

    return MfUltralightPollerCommandContinue;
}

static MfUltralightPollerCommand mf_ultralight_poller_handler_auth(MfUltralightPoller* instance) {
    MfUltralightPollerCommand command = MfUltralightPollerCommandContinue;
    if(instance->feature_set & MfUltralightFeatureSupportAuthentication) {
        MfUltralightPollerEventData event_data = {};
        MfUltralightPollerEvent event = {
            .type = MfUltralightPollerEventTypeAuthRequest,
            .data = &event_data,
        };

        command = instance->callback(event, instance->context);
        if(!event.data->auth_context.skip_auth) {
            instance->auth_context.password = event.data->auth_context.password;
            FURI_LOG_D(
                TAG,
                "Trying to authenticate with password %08lX",
                instance->auth_context.password.pass);
            instance->error = mf_ultralight_poller_async_auth(instance, &instance->auth_context);
            if(instance->error == MfUltralightErrorNone) {
                FURI_LOG_D(TAG, "Auth success");
                instance->auth_context.auth_success = true;
                event.data->auth_context = instance->auth_context;
                event.type = MfUltralightPollerEventTypeAuthSuccess;
                command = instance->callback(event, instance->context);
            } else {
                FURI_LOG_D(TAG, "Auth failed");
                instance->auth_context.auth_success = false;
                MfUltralightPollerEvent event = {.type = MfUltralightPollerEventTypeAuthFailed};
                command = instance->callback(event, instance->context);
                nfca_poller_halt(instance->nfca_poller);
            }
        }
    }
    instance->state = MfUltralightPollerStateReadPages;

    return command;
}

static MfUltralightPollerCommand
    mf_ultralight_poller_handler_read_pages(MfUltralightPoller* instance) {
    MfUltralightPageReadCommandData data = {};
    uint8_t start_page = instance->pages_read;
    instance->error = mf_ultralight_poller_async_read_page(instance, start_page, &data);
    if(instance->error == MfUltralightErrorNone) {
        for(size_t i = 0; i < 4; i++) {
            if(start_page + i < instance->pages_total) {
                FURI_LOG_D(TAG, "Read page %d success", start_page + i);
                instance->data->page[start_page + i] = data.page[i];
                instance->pages_read++;
                instance->data->pages_read = instance->pages_read;
            }
        }
        if(instance->pages_read == instance->pages_total) {
            instance->state = MfUltralightPollerStateReadCounters;
        }
    } else {
        FURI_LOG_D(TAG, "Read page %d failed", instance->pages_read);
        if(instance->pages_read) {
            instance->state = MfUltralightPollerStateReadCounters;
        } else {
            instance->state = MfUltralightPollerStateReadFailed;
        }
    }

    return MfUltralightPollerCommandContinue;
}

static MfUltralightPollerCommand
    mf_ultralight_poller_handler_try_default_pass(MfUltralightPoller* instance) {
    if(instance->feature_set & MfUltralightFeatureSupportAuthentication) {
        MfUltralightConfigPages* config = NULL;
        mf_ultralight_get_config_page(instance->data, &config);
        if(instance->auth_context.auth_success) {
            config->password = instance->auth_context.password;
            config->pack = instance->auth_context.pack;
        } else if(config->access.authlim == 0) {
            FURI_LOG_D(TAG, "No limits in authentication. Trying default password");
            instance->auth_context.password.pass = MF_ULTRALIGHT_DEFAULT_PASSWORD;
            instance->error = mf_ultralight_poller_async_auth(instance, &instance->auth_context);
            if(instance->error == MfUltralightErrorNone) {
                FURI_LOG_D(TAG, "Default password detected");
                config->password.pass = MF_ULTRALIGHT_DEFAULT_PASSWORD;
                config->pack = instance->auth_context.pack;
            }
        }

        if(instance->pages_read != instance->pages_total) {
            // Probably password protected, fix AUTH0 and PROT so before AUTH0
            // can be written and since AUTH0 won't be readable, like on the
            // original card
            config->auth0 = instance->pages_read;
            config->access.prot = true;
        }
    }

    instance->state = MfUltralightPollerStateReadSuccess;
    return MfUltralightPollerCommandContinue;
}

static MfUltralightPollerCommand
    mf_ultralight_poller_handler_read_fail(MfUltralightPoller* instance) {
    FURI_LOG_D(TAG, "Read Failed");
    MfUltralightPollerEventData event_data = {.error = instance->error};
    MfUltralightPollerEvent event = {
        .type = MfUltralightPollerEventTypeReadFailed, .data = &event_data};
    MfUltralightPollerCommand command = instance->callback(event, instance->context);
    instance->state = MfUltralightPollerStateIdle;
    return command;
}

static MfUltralightPollerCommand
    mf_ultralight_poller_handler_read_success(MfUltralightPoller* instance) {
    FURI_LOG_D(TAG, "Read success.");
    MfUltralightPollerEvent event = {.type = MfUltralightPollerEventTypeReadSuccess};
    MfUltralightPollerCommand command = instance->callback(event, instance->context);
    return command;
}

static const MfUltralightPollerReadHandler
    mf_ultralight_poller_read_handler[MfUltralightPollerStateNum] = {
        [MfUltralightPollerStateIdle] = mf_ultralight_poller_handler_idle,
        [MfUltralightPollerStateReadVersion] = mf_ultralight_poller_handler_read_version,
        [MfUltralightPollerStateDetectNtag203] = mf_ultralight_poller_handler_check_ntag_203,
        [MfUltralightPollerStateGetFeatureSet] = mf_ultralight_poller_handler_get_feature_set,
        [MfUltralightPollerStateReadSignature] = mf_ultralight_poller_handler_read_signature,
        [MfUltralightPollerStateReadCounters] = mf_ultralight_poller_handler_read_counters,
        [MfUltralightPollerStateReadTearingFlags] =
            mf_ultralight_poller_handler_read_tearing_flags,
        [MfUltralightPollerStateAuth] = mf_ultralight_poller_handler_auth,
        [MfUltralightPollerStateTryDefaultPass] = mf_ultralight_poller_handler_try_default_pass,
        [MfUltralightPollerStateReadPages] = mf_ultralight_poller_handler_read_pages,
        [MfUltralightPollerStateReadFailed] = mf_ultralight_poller_handler_read_fail,
        [MfUltralightPollerStateReadSuccess] = mf_ultralight_poller_handler_read_success,

};

static NfcaPollerCommand mf_ultralight_poller_read_callback(NfcaPollerEvent event, void* context) {
    furi_assert(context);

    MfUltralightPoller* instance = context;
    MfUltralightPollerEventData event_data = {};
    MfUltralightPollerEvent mf_ul_poller_event = {.data = &event_data};
    MfUltralightPollerCommand command = MfUltralightPollerCommandContinue;

    furi_assert(instance->session_state != MfUltralightPollerSessionStateIdle);
    if(instance->session_state == MfUltralightPollerSessionStateStopRequest) {
        command = MfUltralightPollerCommandStop;
    } else {
        if(event.type == NfcaPollerEventTypeReady) {
            command = mf_ultralight_poller_read_handler[instance->state](instance);
        } else if(event.type == NfcaPollerEventTypeError) {
            if(instance->callback) {
                mf_ul_poller_event.type = MfUltralightPollerEventTypeReadFailed;
                command = instance->callback(mf_ul_poller_event, instance->context);
            }
        }
    }

    return mf_ultralight_process_command(command);
}

MfUltralightError mf_ultralight_poller_start(
    MfUltralightPoller* instance,
    NfcaPollerEventCallback callback,
    void* context) {
    furi_assert(instance);
    furi_assert(instance->state == MfUltralightPollerStateIdle);
    furi_assert(instance->nfca_poller);
    furi_assert(callback);
    furi_assert(instance->session_state == MfUltralightPollerSessionStateIdle);

    instance->data = malloc(sizeof(MfUltralightData));
    instance->buffer =
        nfc_poller_buffer_alloc(MF_ULTRALIGHT_MAX_BUFF_SIZE, MF_ULTRALIGHT_MAX_BUFF_SIZE);

    instance->session_state = MfUltralightPollerSessionStateActive;
    nfca_poller_start(instance->nfca_poller, callback, context);

    return MfUltralightErrorNone;
}

MfUltralightError mf_ultralight_poller_read(
    MfUltralightPoller* instance,
    MfUltralightPollerCallback callback,
    void* context) {
    furi_assert(instance);
    furi_assert(instance->state == MfUltralightPollerStateIdle);
    furi_assert(instance->nfca_poller);
    furi_assert(callback);

    instance->callback = callback;
    instance->context = context;

    return mf_ultralight_poller_start(instance, mf_ultralight_poller_read_callback, instance);
}

MfUltralightError
    mf_ultralight_poller_get_data(MfUltralightPoller* instance, MfUltralightData* data) {
    furi_assert(instance);
    furi_assert(instance->data);
    furi_assert(data);

    *data = *instance->data;

    return MfUltralightErrorNone;
}

MfUltralightError mf_ultralight_poller_reset(MfUltralightPoller* instance) {
    furi_assert(instance);
    furi_assert(instance->data);
    furi_assert(instance->buffer);
    furi_assert(instance->nfca_poller);

    nfc_poller_buffer_free(instance->buffer);
    instance->callback = NULL;
    instance->context = NULL;
    instance->state = MfUltralightPollerStateIdle;

    return MfUltralightErrorNone;
}

MfUltralightError mf_ultralight_poller_stop(MfUltralightPoller* instance) {
    furi_assert(instance);
    furi_assert(instance->nfca_poller);

    instance->session_state = MfUltralightPollerSessionStateStopRequest;
    nfca_poller_stop(instance->nfca_poller);
    instance->session_state = MfUltralightPollerSessionStateIdle;
    free(instance->data);

    return mf_ultralight_poller_reset(instance);
}