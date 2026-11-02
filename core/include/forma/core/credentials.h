#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "forma/core/result.h"

namespace forma {

class ICredentialStore {
public:
    virtual ~ICredentialStore() = default;
    virtual Status set(std::string_view account, std::string_view secret) = 0;
    virtual Result<std::optional<std::string>> get(std::string_view account) = 0;
    virtual Status remove(std::string_view account) = 0;
    virtual const char* backendName() const = 0;
    virtual bool persistent() const = 0;
};

std::unique_ptr<ICredentialStore> createPlatformCredentialStore();
std::unique_ptr<ICredentialStore> createMemoryCredentialStore();

}
