#include "forma/core/credentials.h"

#include <map>
#include <mutex>

#if defined(_WIN32)
#include <windows.h>
#include <wincred.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#elif defined(__linux__)
#include <dlfcn.h>
#endif

namespace forma {

namespace {

constexpr const char* kServiceName = "FormaIDE";

class MemoryCredentialStore final : public ICredentialStore {
public:
    Status set(std::string_view account, std::string_view secret) override
    {
        std::lock_guard lock(mutex_);
        values_[std::string(account)] = std::string(secret);
        return {};
    }

    Result<std::optional<std::string>> get(std::string_view account) override
    {
        std::lock_guard lock(mutex_);
        auto it = values_.find(std::string(account));
        if (it == values_.end())
            return std::optional<std::string>{};
        return std::optional<std::string>(it->second);
    }

    Status remove(std::string_view account) override
    {
        std::lock_guard lock(mutex_);
        values_.erase(std::string(account));
        return {};
    }

    const char* backendName() const override { return "memory (not saved between sessions)"; }
    bool persistent() const override { return false; }

private:
    std::mutex mutex_;
    std::map<std::string, std::string> values_;
};

#if defined(_WIN32)

std::wstring targetName(std::string_view account)
{
    std::string name = std::string(kServiceName) + ":" + std::string(account);
    int length = MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nullptr, 0);
    std::wstring output(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), output.data(), length);
    return output;
}

class WindowsCredentialStore final : public ICredentialStore {
public:
    Status set(std::string_view account, std::string_view secret) override
    {
        std::wstring target = targetName(account);
        CREDENTIALW credential{};
        credential.Type = CRED_TYPE_GENERIC;
        credential.TargetName = target.data();
        credential.CredentialBlobSize = static_cast<DWORD>(secret.size());
        credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(secret.data()));
        credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
        if (!CredWriteW(&credential, 0))
            return makeError("credentials.write", "Windows Credential Manager refused the write");
        return {};
    }

    Result<std::optional<std::string>> get(std::string_view account) override
    {
        std::wstring target = targetName(account);
        PCREDENTIALW credential = nullptr;
        if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential)) {
            if (GetLastError() == ERROR_NOT_FOUND)
                return std::optional<std::string>{};
            return makeError("credentials.read", "Windows Credential Manager refused the read");
        }
        std::string secret(reinterpret_cast<const char*>(credential->CredentialBlob), credential->CredentialBlobSize);
        CredFree(credential);
        return std::optional<std::string>(std::move(secret));
    }

    Status remove(std::string_view account) override
    {
        std::wstring target = targetName(account);
        if (!CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0) && GetLastError() != ERROR_NOT_FOUND)
            return makeError("credentials.delete", "Windows Credential Manager refused the delete");
        return {};
    }

    const char* backendName() const override { return "Windows Credential Manager"; }
    bool persistent() const override { return true; }
};

#elif defined(__APPLE__)

class CfString {
public:
    explicit CfString(std::string_view text)
        : value_(CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(text.data()),
                                         static_cast<CFIndex>(text.size()), kCFStringEncodingUTF8, false))
    {
    }
    ~CfString()
    {
        if (value_ != nullptr)
            CFRelease(value_);
    }
    CFStringRef get() const { return value_; }

private:
    CFStringRef value_;
};

class KeychainCredentialStore final : public ICredentialStore {
public:
    Status set(std::string_view account, std::string_view secret) override
    {
        auto status = remove(account);
        if (!status)
            return status;
        CfString service(kServiceName);
        CfString name(account);
        CFDataRef data = CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(secret.data()), static_cast<CFIndex>(secret.size()));
        const void* keys[] = {kSecClass, kSecAttrService, kSecAttrAccount, kSecValueData};
        const void* values[] = {kSecClassGenericPassword, service.get(), name.get(), data};
        CFDictionaryRef query = CFDictionaryCreate(nullptr, keys, values, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        OSStatus code = SecItemAdd(query, nullptr);
        CFRelease(query);
        CFRelease(data);
        if (code != errSecSuccess)
            return makeError("credentials.write", "The Keychain refused the write");
        return {};
    }

    Result<std::optional<std::string>> get(std::string_view account) override
    {
        CfString service(kServiceName);
        CfString name(account);
        const void* keys[] = {kSecClass, kSecAttrService, kSecAttrAccount, kSecReturnData, kSecMatchLimit};
        const void* values[] = {kSecClassGenericPassword, service.get(), name.get(), kCFBooleanTrue, kSecMatchLimitOne};
        CFDictionaryRef query = CFDictionaryCreate(nullptr, keys, values, 5, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFTypeRef result = nullptr;
        OSStatus code = SecItemCopyMatching(query, &result);
        CFRelease(query);
        if (code == errSecItemNotFound)
            return std::optional<std::string>{};
        if (code != errSecSuccess || result == nullptr)
            return makeError("credentials.read", "The Keychain refused the read");
        auto data = static_cast<CFDataRef>(result);
        std::string secret(reinterpret_cast<const char*>(CFDataGetBytePtr(data)), static_cast<std::size_t>(CFDataGetLength(data)));
        CFRelease(result);
        return std::optional<std::string>(std::move(secret));
    }

    Status remove(std::string_view account) override
    {
        CfString service(kServiceName);
        CfString name(account);
        const void* keys[] = {kSecClass, kSecAttrService, kSecAttrAccount};
        const void* values[] = {kSecClassGenericPassword, service.get(), name.get()};
        CFDictionaryRef query = CFDictionaryCreate(nullptr, keys, values, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        OSStatus code = SecItemDelete(query);
        CFRelease(query);
        if (code != errSecSuccess && code != errSecItemNotFound)
            return makeError("credentials.delete", "The Keychain refused the delete");
        return {};
    }

    const char* backendName() const override { return "macOS Keychain"; }
    bool persistent() const override { return true; }
};

#elif defined(__linux__)

struct SecretSchemaAttribute {
    const char* name;
    int type;
};

struct SecretSchema {
    const char* name;
    int flags;
    SecretSchemaAttribute attributes[32];
    int reserved;
    void* reserved1;
    void* reserved2;
    void* reserved3;
    void* reserved4;
    void* reserved5;
    void* reserved6;
    void* reserved7;
};

struct GError {
    unsigned int domain;
    int code;
    char* message;
};

class SecretServiceStore final : public ICredentialStore {
public:
    using StoreFn = int (*)(const SecretSchema*, const char*, const char*, const char*, void*, GError**, ...);
    using LookupFn = char* (*)(const SecretSchema*, void*, GError**, ...);
    using ClearFn = int (*)(const SecretSchema*, void*, GError**, ...);
    using FreeFn = void (*)(char*);
    using ErrorFreeFn = void (*)(GError*);

    SecretServiceStore(void* library, StoreFn store, LookupFn lookup, ClearFn clear, FreeFn free, ErrorFreeFn errorFree)
        : library_(library), store_(store), lookup_(lookup), clear_(clear), free_(free), errorFree_(errorFree)
    {
        schema_.name = "org.formaide.Credential";
        schema_.flags = 0;
        schema_.attributes[0] = {"account", 0};
        schema_.attributes[1] = {nullptr, 0};
    }

    ~SecretServiceStore() override { dlclose(library_); }

    Status set(std::string_view account, std::string_view secret) override
    {
        GError* error = nullptr;
        std::string name(account);
        std::string value(secret);
        std::string label = std::string(kServiceName) + " " + name;
        int ok = store_(&schema_, "default", label.c_str(), value.c_str(), nullptr, &error, "account", name.c_str(), nullptr);
        return finish(ok != 0, error, "credentials.write");
    }

    Result<std::optional<std::string>> get(std::string_view account) override
    {
        GError* error = nullptr;
        std::string name(account);
        char* value = lookup_(&schema_, nullptr, &error, "account", name.c_str(), nullptr);
        if (error != nullptr) {
            auto status = finish(false, error, "credentials.read");
            return status.error();
        }
        if (value == nullptr)
            return std::optional<std::string>{};
        std::string secret(value);
        free_(value);
        return std::optional<std::string>(std::move(secret));
    }

    Status remove(std::string_view account) override
    {
        GError* error = nullptr;
        std::string name(account);
        clear_(&schema_, nullptr, &error, "account", name.c_str(), nullptr);
        return finish(error == nullptr, error, "credentials.delete");
    }

    const char* backendName() const override { return "Secret Service"; }
    bool persistent() const override { return true; }

private:
    Status finish(bool ok, GError* error, const char* code)
    {
        if (ok && error == nullptr)
            return {};
        std::string message = error != nullptr && error->message != nullptr ? error->message : "Secret Service error";
        if (error != nullptr)
            errorFree_(error);
        return makeError(code, message);
    }

    void* library_;
    StoreFn store_;
    LookupFn lookup_;
    ClearFn clear_;
    FreeFn free_;
    ErrorFreeFn errorFree_;
    SecretSchema schema_{};
};

#endif

}

std::unique_ptr<ICredentialStore> createMemoryCredentialStore()
{
    return std::make_unique<MemoryCredentialStore>();
}

std::unique_ptr<ICredentialStore> createPlatformCredentialStore()
{
#if defined(_WIN32)
    return std::make_unique<WindowsCredentialStore>();
#elif defined(__APPLE__)
    return std::make_unique<KeychainCredentialStore>();
#elif defined(__linux__)
    void* library = dlopen("libsecret-1.so.0", RTLD_NOW | RTLD_LOCAL);
    void* glib = dlopen("libglib-2.0.so.0", RTLD_NOW | RTLD_LOCAL);
    if (library != nullptr && glib != nullptr) {
        auto store = reinterpret_cast<SecretServiceStore::StoreFn>(dlsym(library, "secret_password_store_sync"));
        auto lookup = reinterpret_cast<SecretServiceStore::LookupFn>(dlsym(library, "secret_password_lookup_sync"));
        auto clear = reinterpret_cast<SecretServiceStore::ClearFn>(dlsym(library, "secret_password_clear_sync"));
        auto free = reinterpret_cast<SecretServiceStore::FreeFn>(dlsym(library, "secret_password_free"));
        auto errorFree = reinterpret_cast<SecretServiceStore::ErrorFreeFn>(dlsym(glib, "g_error_free"));
        if (store != nullptr && lookup != nullptr && clear != nullptr && free != nullptr && errorFree != nullptr)
            return std::make_unique<SecretServiceStore>(library, store, lookup, clear, free, errorFree);
    }
    return std::make_unique<MemoryCredentialStore>();
#else
    return std::make_unique<MemoryCredentialStore>();
#endif
}

}
