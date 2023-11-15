#pragma once

#include "store_definition.hpp"

namespace config_store_ns {
namespace deprecated_ids {
    inline constexpr uint16_t selftest_result_pre_23[] {
        journal::hash("Selftest Result"),
    };
    inline constexpr uint16_t selftest_result_pre_gears[] {
        journal::hash("Selftest Result V23"),
    };
    inline constexpr uint16_t fsensor_enabled_v1[] {
        journal::hash("FSensor Enabled"),
    };
} // namespace deprecated_ids

namespace migrations {
    // Commented thoroughly to be used as an example for more migrations.
    void selftest_result_pre_23(journal::Backend &backend);
    void selftest_result_pre_gears(journal::Backend &backend);
    void fsensor_enabled_v1(journal::Backend &backend);
} // namespace migrations

/**
 * @brief This array holds previous versions of the configuration store.
 *
 */
inline constexpr journal::Backend::MigrationFunction migration_functions[] {
    { migrations::selftest_result_pre_23, deprecated_ids::selftest_result_pre_23 },
    { migrations::selftest_result_pre_gears, deprecated_ids::selftest_result_pre_gears },
    { migrations::fsensor_enabled_v1, deprecated_ids::fsensor_enabled_v1 },
};

// Span of migration versions to simplify passing it around
inline constexpr std::span<const journal::Backend::MigrationFunction> migration_functions_span { migration_functions };
} // namespace config_store_ns
