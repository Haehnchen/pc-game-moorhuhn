#pragma once

#include <filesystem>
#include <optional>

namespace moorhuhn::platform {

class AppPaths {
public:
    // No override uses the executable directory. Explicit roots must be absolute.
    [[nodiscard]] static AppPaths discover(std::optional<std::filesystem::path> resource_root = {});

    [[nodiscard]] const std::filesystem::path& resource_root() const noexcept {
        return resource_root_;
    }

    [[nodiscard]] const std::filesystem::path& user_root() const noexcept {
        return user_root_;
    }

    [[nodiscard]] std::filesystem::path resource(const std::filesystem::path& relative) const;
    [[nodiscard]] std::filesystem::path user_file(const std::filesystem::path& relative) const;

private:
    std::filesystem::path resource_root_;
    std::filesystem::path user_root_;
};

} // namespace moorhuhn::platform
