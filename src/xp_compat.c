#include <windows.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

// BCrypt fallbacks for Windows XP (using CryptoAPI)
NTSTATUS WINAPI BCryptGenRandom(BCRYPT_ALG_HANDLE hAlgorithm, PUCHAR pbBuffer, ULONG cbBuffer, ULONG dwFlags) {
    (void)hAlgorithm;
    (void)dwFlags;
    HCRYPTPROV hProv = 0;
    if (CryptAcquireContextA(&hProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
        BOOL ok = CryptGenRandom(hProv, cbBuffer, pbBuffer);
        CryptReleaseContext(hProv, 0);
        return ok ? 0 : 0xC0000001;
    }
    return 0xC0000001;
}

NTSTATUS WINAPI BCryptOpenAlgorithmProvider(BCRYPT_ALG_HANDLE *phAlgorithm, LPCWSTR pszAlgId, LPCWSTR pszImplementation, ULONG dwFlags) {
    (void)pszAlgId;
    (void)pszImplementation;
    (void)dwFlags;
    if (phAlgorithm) *phAlgorithm = (BCRYPT_ALG_HANDLE)0x1234;
    return 0;
}

NTSTATUS WINAPI BCryptCloseAlgorithmProvider(BCRYPT_ALG_HANDLE hAlgorithm, ULONG dwFlags) {
    (void)hAlgorithm;
    (void)dwFlags;
    return 0;
}

// MSVCR80+ Secure CRT fallback implementations for vanilla Windows XP msvcrt.dll
errno_t mbstowcs_s_impl(size_t* pRetVal, wchar_t* dst, size_t dstSize, const char* src, size_t count) asm("mbstowcs_s");
errno_t mbstowcs_s_impl(size_t* pRetVal, wchar_t* dst, size_t dstSize, const char* src, size_t count) {
    if (!src || (!dst && dstSize > 0)) return EINVAL;
    size_t converted = mbstowcs(dst, src, dst ? dstSize : 0);
    if (converted == (size_t)-1) return EILSEQ;
    if (dst && dstSize > 0) {
        if (converted >= dstSize) {
            dst[dstSize - 1] = L'\0';
            if (pRetVal) *pRetVal = dstSize;
            return ERANGE;
        }
        dst[converted] = L'\0';
    }
    if (pRetVal) *pRetVal = converted + 1;
    return 0;
}

char* strtok_s_impl(char* str, const char* delim, char** saveptr) asm("strtok_s");
char* strtok_s_impl(char* str, const char* delim, char** saveptr) {
    if (!saveptr || !delim) return NULL;
    if (!str) str = *saveptr;
    if (!str) return NULL;

    str += strspn(str, delim);
    if (*str == '\0') {
        *saveptr = NULL;
        return NULL;
    }

    char* end = str + strcspn(str, delim);
    if (*end == '\0') {
        *saveptr = NULL;
    } else {
        *end = '\0';
        *saveptr = end + 1;
    }
    return str;
}

int vsnprintf_s_impl(char* dst, size_t dstSize, size_t maxCount, const char* format, va_list argList) asm("_vsnprintf_s");
int vsnprintf_s_impl(char* dst, size_t dstSize, size_t maxCount, const char* format, va_list argList) {
    if (!dst || dstSize == 0 || !format) return -1;
    size_t limit = (maxCount < dstSize) ? maxCount : (dstSize - 1);
    int ret = _vsnprintf(dst, limit, format, argList);
    if (ret < 0 || (size_t)ret >= dstSize) {
        dst[dstSize - 1] = '\0';
        return -1;
    }
    dst[ret] = '\0';
    return ret;
}

int snprintf_s_impl(char* dst, size_t dstSize, size_t maxCount, const char* format, ...) asm("_snprintf_s");
int snprintf_s_impl(char* dst, size_t dstSize, size_t maxCount, const char* format, ...) {
    va_list args;
    va_start(args, format);
    int ret = vsnprintf_s_impl(dst, dstSize, maxCount, format, args);
    va_end(args);
    return ret;
}

// Override import table pointers to prevent dynamic linkage to msvcrt.dll
void* _imp__mbstowcs_s asm("__imp__mbstowcs_s") = (void*)mbstowcs_s_impl;
void* _imp__strtok_s asm("__imp__strtok_s") = (void*)strtok_s_impl;
void* _imp___vsnprintf_s asm("__imp___vsnprintf_s") = (void*)vsnprintf_s_impl;
void* _imp___snprintf_s asm("__imp___snprintf_s") = (void*)snprintf_s_impl;
