/**
 * @file selftest_axis_interface.cpp
 * @author Radek Vana
 * @date 2021-09-24
 */
#include "selftest_fsensor_interface.hpp"
#include "selftest_fsensor.h"
#include "selftest_sub_state.hpp"
#include "marlin_server.hpp"
#include "selftest_part.hpp"
#include "selftest_tool_helper.hpp"
#if BOARD_IS_XLBUDDY
    #include "src/module/prusa/toolchanger.h"
#endif
#include <configuration_store.hpp>

namespace selftest {
static SelftestFSensor_t staticResult; // automatically initialized by PartHandler

TestReturn phaseFSensor(const uint8_t tool_mask, std::array<IPartHandler *, HOTENDS> &m_pFSensor, const std::array<const FSensorConfig_t, HOTENDS> &configs) {
    for (uint i = 0; i < HOTENDS; ++i) {
        if (!is_tool_selftest_enabled(i, tool_mask)) {
            continue;
        }

        if (!m_pFSensor[i]) {
            // clang-format off
            m_pFSensor[i] = selftest::Factory::CreateDynamical<CSelftestPart_FSensor>(
                configs[i],
                staticResult,
                &CSelftestPart_FSensor::state_init,
                &CSelftestPart_FSensor::state_wait_tool_pick,
                &CSelftestPart_FSensor::state_ask_unload_init,
                &CSelftestPart_FSensor::state_ask_unload_wait,
                &CSelftestPart_FSensor::state_filament_unload_enqueue_gcode,
                &CSelftestPart_FSensor::state_filament_unload_wait_finished,
                &CSelftestPart_FSensor::state_ask_unload_confirm_init,
                &CSelftestPart_FSensor::state_ask_unload_confirm_wait,
                &CSelftestPart_FSensor::state_calibrate,
                &CSelftestPart_FSensor::state_calibrate_wait_finished,
                &CSelftestPart_FSensor::state_insertion_wait_init,
                &CSelftestPart_FSensor::state_insertion_wait,
                &CSelftestPart_FSensor::state_insertion_ok_init,
                &CSelftestPart_FSensor::state_insertion_ok,
                &CSelftestPart_FSensor::state_insertion_calibrate_start,
                &CSelftestPart_FSensor::state_insertion_calibrate_wait,
                &CSelftestPart_FSensor::state_enforce_remove_init,
                &CSelftestPart_FSensor::state_enforce_remove
            );
            // clang-format on
        }
    }

    bool in_progress = false;
    for (uint i = 0; i < HOTENDS; ++i) {
        if (!is_tool_selftest_enabled(i, tool_mask)) {
            continue;
        }

        in_progress = in_progress || m_pFSensor[i]->Loop();
    }
    FSM_CHANGE_WITH_DATA__LOGGING(Selftest, IPartHandler::GetFsmPhase(), staticResult.Serialize());

    if (in_progress) {
        return true;
    }

    bool skipped = false; ///< Return value whether to run next test
    SelftestResult eeres = config_store().selftest_result.get();
    for (uint i = 0; i < HOTENDS; ++i) {
        if (!is_tool_selftest_enabled(i, tool_mask)) {
            continue;
        }

        // Store filament sensor calibration state
        // Do not store if test was successful and now aborted, do not regress
        if (i < EEPROM_MAX_TOOL_COUNT
            && !(eeres.tools[i].fsensor == TestResult_Passed && m_pFSensor[i]->GetResult() == TestResult_Skipped)) {
            eeres.tools[i].fsensor = m_pFSensor[i]->GetResult();
        }

        // If any test failed, do not run next test
        if (m_pFSensor[i]->GetResult() != TestResult_Passed) {
            skipped = true;
        }

        delete m_pFSensor[i];
        m_pFSensor[i] = nullptr;
    }
    config_store().selftest_result.set(eeres);

    return TestReturn(false, skipped);
}
} // namespace selftest
