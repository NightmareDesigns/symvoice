#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <sapi.h>
#include <sapiddk.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SPEECH_CHARS 65536U
#define MAX_RESPONSE_BYTES (64U * 1024U * 1024U)
#define API_HOST L"127.0.0.1"
#define API_PORT 8021
#define API_PATH L"/v1/audio/speech"

static const CLSID CLSID_SymvoiceEngine = {
    0x73ea0e66, 0x7b92, 0x4d89, {0x8b, 0xe0, 0xa6, 0x33, 0xf9, 0x27, 0x42, 0x91}
};

static LONG g_dll_references;

typedef struct {
    IClassFactory interface;
    LONG references;
} symvoice_factory;

static HRESULT STDMETHODCALLTYPE engine_query_interface(
    ISpTTSEngine *interface, REFIID iid, void **object);
static ULONG STDMETHODCALLTYPE engine_add_ref(ISpTTSEngine *interface);
static ULONG STDMETHODCALLTYPE engine_release(ISpTTSEngine *interface);
static HRESULT STDMETHODCALLTYPE engine_speak(
    ISpTTSEngine *interface,
    DWORD flags,
    REFGUID format_id,
    const WAVEFORMATEX *format,
    const SPVTEXTFRAG *fragments,
    ISpTTSEngineSite *site);
static HRESULT STDMETHODCALLTYPE engine_get_output_format(
    ISpTTSEngine *interface,
    const GUID *target_format_id,
    const WAVEFORMATEX *target_format,
    GUID *output_format_id,
    WAVEFORMATEX **output_format);

static const ISpTTSEngineVtbl g_engine_vtable = {
    engine_query_interface,
    engine_add_ref,
    engine_release,
    engine_speak,
    engine_get_output_format
};

typedef struct {
    ISpObjectWithToken interface;
} symvoice_token_interface;

static symvoice_token_interface *token_interface_from_interface(ISpObjectWithToken *interface)
{
    return (symvoice_token_interface *)((char *)interface
        - offsetof(symvoice_token_interface, interface));
}

static HRESULT STDMETHODCALLTYPE token_query_interface(
    ISpObjectWithToken *interface, REFIID iid, void **object);
static ULONG STDMETHODCALLTYPE token_add_ref(ISpObjectWithToken *interface);
static ULONG STDMETHODCALLTYPE token_release(ISpObjectWithToken *interface);
static HRESULT STDMETHODCALLTYPE token_set(
    ISpObjectWithToken *interface, ISpObjectToken *token);
static HRESULT STDMETHODCALLTYPE token_get(
    ISpObjectWithToken *interface, ISpObjectToken **token);

static const ISpObjectWithTokenVtbl g_token_vtable = {
    token_query_interface,
    token_add_ref,
    token_release,
    token_set,
    token_get
};

typedef struct {
    ISpTTSEngine engine;
    symvoice_token_interface token;
    LONG references;
    ISpObjectToken *object_token;
} symvoice_object;

static symvoice_object *object_from_engine(ISpTTSEngine *interface)
{
    return (symvoice_object *)((char *)interface - offsetof(symvoice_object, engine));
}

static symvoice_object *object_from_token(ISpObjectWithToken *interface)
{
    symvoice_token_interface *token = token_interface_from_interface(interface);
    return (symvoice_object *)((char *)token - offsetof(symvoice_object, token));
}

static HRESULT STDMETHODCALLTYPE object_query_interface(
    symvoice_object *object, REFIID iid, void **result)
{
    if (result == NULL) {
        return E_POINTER;
    }
    *result = NULL;
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_ISpTTSEngine)) {
        *result = &object->engine;
    } else if (IsEqualIID(iid, &IID_ISpObjectWithToken)) {
        *result = &object->token.interface;
    } else {
        return E_NOINTERFACE;
    }
    InterlockedIncrement(&object->references);
    return S_OK;
}

static ULONG object_add_ref(symvoice_object *object)
{
    return (ULONG)InterlockedIncrement(&object->references);
}

static ULONG object_release(symvoice_object *object)
{
    LONG references = InterlockedDecrement(&object->references);
    if (references == 0) {
        if (object->object_token != NULL) {
            ISpObjectToken_Release(object->object_token);
        }
        InterlockedDecrement(&g_dll_references);
        CoTaskMemFree(object);
    }
    return (ULONG)references;
}

static HRESULT STDMETHODCALLTYPE engine_query_interface(
    ISpTTSEngine *interface, REFIID iid, void **object)
{
    return object_query_interface(object_from_engine(interface), iid, object);
}

static ULONG STDMETHODCALLTYPE engine_add_ref(ISpTTSEngine *interface)
{
    return object_add_ref(object_from_engine(interface));
}

static ULONG STDMETHODCALLTYPE engine_release(ISpTTSEngine *interface)
{
    return object_release(object_from_engine(interface));
}

static HRESULT STDMETHODCALLTYPE token_query_interface(
    ISpObjectWithToken *interface, REFIID iid, void **object)
{
    return object_query_interface(object_from_token(interface), iid, object);
}

static ULONG STDMETHODCALLTYPE token_add_ref(ISpObjectWithToken *interface)
{
    return object_add_ref(object_from_token(interface));
}

static ULONG STDMETHODCALLTYPE token_release(ISpObjectWithToken *interface)
{
    return object_release(object_from_token(interface));
}

static HRESULT STDMETHODCALLTYPE token_set(
    ISpObjectWithToken *interface, ISpObjectToken *token)
{
    symvoice_object *object = object_from_token(interface);
    if (object->object_token != NULL) {
        ISpObjectToken_Release(object->object_token);
    }
    object->object_token = token;
    if (token != NULL) {
        ISpObjectToken_AddRef(token);
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE token_get(
    ISpObjectWithToken *interface, ISpObjectToken **token)
{
    symvoice_object *object = object_from_token(interface);
    if (token == NULL) {
        return E_POINTER;
    }
    *token = object->object_token;
    if (*token != NULL) {
        ISpObjectToken_AddRef(*token);
    }
    return S_OK;
}

static HRESULT json_escape_utf8(const char *text, DWORD length, char **json, DWORD *json_length)
{
    static const char hex[] = "0123456789abcdef";
    static const char suffix[] = "\",\"response_format\":\"wav\"}";
    size_t capacity = (size_t)length * 6U + 32U;
    char *result = (char *)HeapAlloc(GetProcessHeap(), 0, capacity);
    size_t used = 0;

    if (result == NULL) {
        return E_OUTOFMEMORY;
    }
    result[used++] = '{';
    memcpy(result + used, "\"input\":\"", 9);
    used += 9;
    for (DWORD index = 0; index < length; ++index) {
        unsigned char ch = (unsigned char)text[index];
        if (ch == '"' || ch == '\\') {
            result[used++] = '\\';
            result[used++] = (char)ch;
        } else if (ch < 0x20U) {
            result[used++] = '\\';
            result[used++] = 'u';
            result[used++] = '0';
            result[used++] = '0';
            result[used++] = hex[ch >> 4];
            result[used++] = hex[ch & 0xfU];
        } else {
            result[used++] = (char)ch;
        }
    }
    memcpy(result + used, suffix, sizeof(suffix) - 1U);
    used += sizeof(suffix) - 1U;
    *json = result;
    *json_length = (DWORD)used;
    return S_OK;
}

static HRESULT request_speech(const WCHAR *text, DWORD text_length, BYTE **audio, DWORD *audio_length)
{
    HINTERNET session = NULL;
    HINTERNET connection = NULL;
    HINTERNET request = NULL;
    char *utf8 = NULL;
    char *json = NULL;
    BYTE *response = NULL;
    DWORD response_size = 0;
    DWORD response_capacity = 0;
    HRESULT status = E_FAIL;
    int utf8_length;
    DWORD json_length = 0;
    DWORD http_status = 0;
    DWORD status_size = sizeof(http_status);

    if (text_length == 0 || text_length > MAX_SPEECH_CHARS) {
        return E_INVALIDARG;
    }
    utf8_length = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text, (int)text_length, NULL, 0, NULL, NULL);
    if (utf8_length <= 0) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    utf8 = (char *)HeapAlloc(GetProcessHeap(), 0, (size_t)utf8_length + 1U);
    if (utf8 == NULL) {
        status = E_OUTOFMEMORY;
        goto cleanup;
    }
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, text, (int)text_length,
            utf8, utf8_length, NULL, NULL) != utf8_length) {
        status = HRESULT_FROM_WIN32(GetLastError());
        goto cleanup;
    }
    utf8[utf8_length] = '\0';
    status = json_escape_utf8(utf8, (DWORD)utf8_length, &json, &json_length);
    if (FAILED(status)) {
        goto cleanup;
    }

    session = WinHttpOpen(L"Symvoice-SAPI/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == NULL) {
        status = HRESULT_FROM_WIN32(GetLastError());
        goto cleanup;
    }
    WinHttpSetTimeouts(session, 3000, 3000, 5000, 30000);
    connection = WinHttpConnect(session, API_HOST, API_PORT, 0);
    if (connection == NULL) {
        status = HRESULT_FROM_WIN32(GetLastError());
        goto cleanup;
    }
    request = WinHttpOpenRequest(
        connection, L"POST", API_PATH, NULL, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (request == NULL) {
        status = HRESULT_FROM_WIN32(GetLastError());
        goto cleanup;
    }
    if (!WinHttpSendRequest(
            request, L"Content-Type: application/json\r\n", (DWORD)-1L,
            json, json_length, json_length, 0)
        || !WinHttpReceiveResponse(request, NULL)
        || !WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &http_status, &status_size,
                                WINHTTP_NO_HEADER_INDEX)) {
        status = HRESULT_FROM_WIN32(GetLastError());
        goto cleanup;
    }
    if (http_status != 200) {
        status = HRESULT_FROM_WIN32(ERROR_NOT_READY);
        goto cleanup;
    }

    for (;;) {
        DWORD available = 0;
        DWORD received = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            status = HRESULT_FROM_WIN32(GetLastError());
            goto cleanup;
        }
        if (available == 0) {
            break;
        }
        if (available > MAX_RESPONSE_BYTES - response_size) {
            status = HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
            goto cleanup;
        }
        if (response_size + available > response_capacity) {
            DWORD capacity = response_capacity == 0 ? 65536U : response_capacity;
            while (capacity < response_size + available) {
                if (capacity > MAX_RESPONSE_BYTES / 2U) {
                    capacity = MAX_RESPONSE_BYTES;
                    break;
                }
                capacity *= 2U;
            }
            BYTE *grown = response == NULL
                ? (BYTE *)HeapAlloc(GetProcessHeap(), 0, capacity)
                : (BYTE *)HeapReAlloc(GetProcessHeap(), 0, response, capacity);
            if (grown == NULL) {
                status = E_OUTOFMEMORY;
                goto cleanup;
            }
            response = grown;
            response_capacity = capacity;
        }
        if (!WinHttpReadData(request, response + response_size, available, &received)
            || received == 0) {
            status = HRESULT_FROM_WIN32(GetLastError());
            goto cleanup;
        }
        response_size += received;
    }
    if (response_size < 44U
        || memcmp(response, "RIFF", 4) != 0
        || response[4] != (BYTE)((response_size - 8U) & 0xffU)
        || response[5] != (BYTE)(((response_size - 8U) >> 8) & 0xffU)
        || response[6] != (BYTE)(((response_size - 8U) >> 16) & 0xffU)
        || response[7] != (BYTE)(((response_size - 8U) >> 24) & 0xffU)
        || memcmp(response + 8, "WAVEfmt ", 8) != 0
        || response[16] != 16 || response[17] != 0
        || memcmp(response + 36, "data", 4) != 0
        || response[20] != 1 || response[21] != 0
        || response[22] != 1 || response[23] != 0
        || response[24] != 0x80 || response[25] != 0x3e
        || response[26] != 0 || response[27] != 0
        || response[28] != 0 || response[29] != 0x7d
        || response[30] != 0 || response[31] != 0
        || response[32] != 2 || response[33] != 0
        || response[34] != 16 || response[35] != 0) {
        status = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        goto cleanup;
    }
    {
        DWORD data_size = (DWORD)response[40] | ((DWORD)response[41] << 8)
            | ((DWORD)response[42] << 16) | ((DWORD)response[43] << 24);
        if (data_size == 0 || data_size != response_size - 44U) {
            status = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            goto cleanup;
        }
    }
    *audio = response;
    *audio_length = response_size;
    response = NULL;
    status = S_OK;

cleanup:
    if (request != NULL) WinHttpCloseHandle(request);
    if (connection != NULL) WinHttpCloseHandle(connection);
    if (session != NULL) WinHttpCloseHandle(session);
    if (utf8 != NULL) HeapFree(GetProcessHeap(), 0, utf8);
    if (json != NULL) HeapFree(GetProcessHeap(), 0, json);
    if (response != NULL) HeapFree(GetProcessHeap(), 0, response);
    return status;
}

static HRESULT STDMETHODCALLTYPE engine_speak(
    ISpTTSEngine *interface,
    DWORD flags,
    REFGUID format_id,
    const WAVEFORMATEX *format,
    const SPVTEXTFRAG *fragments,
    ISpTTSEngineSite *site)
{
    WCHAR *text = NULL;
    DWORD text_length = 0;
    DWORD text_capacity = 0;
    BYTE *audio = NULL;
    DWORD audio_length = 0;
    HRESULT status;
    (void)interface;
    (void)flags;
    (void)format_id;
    (void)format;

    if (site == NULL || fragments == NULL) {
        return E_INVALIDARG;
    }
    for (const SPVTEXTFRAG *fragment = fragments; fragment != NULL; fragment = fragment->pNext) {
        DWORD fragment_length = fragment->ulTextLen;
        if (fragment_length == 0 || fragment->pTextStart == NULL) {
            continue;
        }
        if (fragment_length > MAX_SPEECH_CHARS - text_length) {
            status = E_INVALIDARG;
            goto cleanup;
        }
        if (text_length + fragment_length + 1U > text_capacity) {
            DWORD capacity = text_capacity == 0 ? 256U : text_capacity;
            while (capacity < text_length + fragment_length + 1U) {
                capacity *= 2U;
            }
            WCHAR *grown = text == NULL
                ? (WCHAR *)HeapAlloc(
                    GetProcessHeap(), 0, (size_t)capacity * sizeof(WCHAR))
                : (WCHAR *)HeapReAlloc(
                    GetProcessHeap(), 0, text, (size_t)capacity * sizeof(WCHAR));
            if (grown == NULL) {
                status = E_OUTOFMEMORY;
                goto cleanup;
            }
            text = grown;
            text_capacity = capacity;
        }
        memcpy(text + text_length, fragment->pTextStart, (size_t)fragment_length * sizeof(WCHAR));
        text_length += fragment_length;
    }
    if (text_length == 0) {
        return S_OK;
    }
    text[text_length] = L'\0';
    status = request_speech(text, text_length, &audio, &audio_length);
    if (FAILED(status)) {
        goto cleanup;
    }

    for (DWORD offset = 44U; offset < audio_length;) {
        ULONG written = 0;
        ULONG chunk = audio_length - offset > 16384U ? 16384U : audio_length - offset;
        status = site->lpVtbl->Write(site, audio + offset, chunk, &written);
        if (FAILED(status)) {
            goto cleanup;
        }
        if (written == 0 || written > chunk) {
            status = HRESULT_FROM_WIN32(ERROR_CANCELLED);
            goto cleanup;
        }
        offset += written;
    }
    status = S_OK;

cleanup:
    if (text != NULL) HeapFree(GetProcessHeap(), 0, text);
    if (audio != NULL) HeapFree(GetProcessHeap(), 0, audio);
    return status;
}

static HRESULT STDMETHODCALLTYPE engine_get_output_format(
    ISpTTSEngine *interface,
    const GUID *target_format_id,
    const WAVEFORMATEX *target_format,
    GUID *output_format_id,
    WAVEFORMATEX **output_format)
{
    WAVEFORMATEX *format;
    (void)interface;
    (void)target_format_id;
    (void)target_format;
    if (output_format_id == NULL || output_format == NULL) {
        return E_POINTER;
    }
    *output_format = NULL;
    format = (WAVEFORMATEX *)CoTaskMemAlloc(sizeof(*format));
    if (format == NULL) {
        return E_OUTOFMEMORY;
    }
    format->wFormatTag = WAVE_FORMAT_PCM;
    format->nChannels = 1;
    format->nSamplesPerSec = 16000;
    format->wBitsPerSample = 16;
    format->nBlockAlign = 2;
    format->nAvgBytesPerSec = 32000;
    format->cbSize = 0;
    *output_format_id = SPDFID_WaveFormatEx;
    *output_format = format;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE factory_query_interface(
    IClassFactory *interface, REFIID iid, void **object);
static ULONG STDMETHODCALLTYPE factory_add_ref(IClassFactory *interface);
static ULONG STDMETHODCALLTYPE factory_release(IClassFactory *interface);
static HRESULT STDMETHODCALLTYPE factory_create_instance(
    IClassFactory *interface, IUnknown *outer, REFIID iid, void **object);
static HRESULT STDMETHODCALLTYPE factory_lock_server(IClassFactory *interface, BOOL lock);

static const IClassFactoryVtbl g_factory_vtable = {
    factory_query_interface,
    factory_add_ref,
    factory_release,
    factory_create_instance,
    factory_lock_server
};

static symvoice_factory g_factory = {{&g_factory_vtable}, 0};

static HRESULT STDMETHODCALLTYPE factory_query_interface(
    IClassFactory *interface, REFIID iid, void **object)
{
    if (object == NULL) return E_POINTER;
    *object = NULL;
    if (!IsEqualIID(iid, &IID_IUnknown) && !IsEqualIID(iid, &IID_IClassFactory)) {
        return E_NOINTERFACE;
    }
    *object = interface;
    factory_add_ref(interface);
    return S_OK;
}

static ULONG STDMETHODCALLTYPE factory_add_ref(IClassFactory *interface)
{
    symvoice_factory *factory = (symvoice_factory *)interface;
    InterlockedIncrement(&g_dll_references);
    return (ULONG)InterlockedIncrement(&factory->references);
}

static ULONG STDMETHODCALLTYPE factory_release(IClassFactory *interface)
{
    symvoice_factory *factory = (symvoice_factory *)interface;
    LONG references = InterlockedDecrement(&factory->references);
    InterlockedDecrement(&g_dll_references);
    return (ULONG)references;
}

static HRESULT STDMETHODCALLTYPE factory_create_instance(
    IClassFactory *interface, IUnknown *outer, REFIID iid, void **result)
{
    symvoice_object *object;
    HRESULT status;
    (void)interface;
    if (outer != NULL) return CLASS_E_NOAGGREGATION;
    if (result == NULL) return E_POINTER;
    *result = NULL;
    object = (symvoice_object *)CoTaskMemAlloc(sizeof(*object));
    if (object == NULL) return E_OUTOFMEMORY;
    memset(object, 0, sizeof(*object));
    object->engine.lpVtbl = &g_engine_vtable;
    object->token.interface.lpVtbl = &g_token_vtable;
    object->references = 1;
    InterlockedIncrement(&g_dll_references);
    status = object_query_interface(object, iid, result);
    object_release(object);
    return status;
}

static HRESULT STDMETHODCALLTYPE factory_lock_server(IClassFactory *interface, BOOL lock)
{
    (void)interface;
    if (lock) {
        InterlockedIncrement(&g_dll_references);
    } else {
        InterlockedDecrement(&g_dll_references);
    }
    return S_OK;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void **object)
{
    if (!IsEqualCLSID(clsid, &CLSID_SymvoiceEngine)) return CLASS_E_CLASSNOTAVAILABLE;
    return factory_query_interface(&g_factory.interface, iid, object);
}

STDAPI DllCanUnloadNow(void)
{
    return InterlockedCompareExchange(&g_dll_references, 0, 0) == 0 ? S_OK : S_FALSE;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
