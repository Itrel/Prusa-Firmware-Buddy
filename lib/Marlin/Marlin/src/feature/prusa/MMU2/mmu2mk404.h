#pragma once
#include "../../inc/MarlinConfig.h"
#include "protocol_logic.h"
#include "mmu_state.h"
#include "../../Marlin/src/core/macros.h"
#include "../../Marlin/src/core/types.h"
#include <atomic>

#if HAS_MMU2

struct E_Step;

namespace MMU2 {

/// Top-level interface between Logic and Marlin.
/// Intentionally named MMU2 to be (almost) a drop-in replacement for the previous implementation.
/// Most of the public methods share the original naming convention as well.
class MMU2 {
public:
    MMU2();

    /// Powers ON the MMU, then initializes the UART and protocol logic
    void Start();

    /// Stops the protocol logic, closes the UART, powers OFF the MMU
    void Stop();

    inline enum State_t State() const { return state; }

    /// Different levels of resetting the MMU
    enum ResetForm : uint8_t {
        Software = 0,   ///< sends a X0 command into the MMU, the MMU will watchdog-reset itself
        ResetPin = 1,   ///< trigger the reset pin of the MMU
        CutThePower = 2 ///< power off and power on (that includes +5V and +24V power lines)
    };

    /// Perform a reset of the MMU
    /// @param level physical form of the reset
    void Reset(ResetForm level);

    /// Power off the MMU (cut the power)
    void PowerOff();

    /// Power on the MMU
    void PowerOn();

    /// The main loop of MMU processing.
    /// Doesn't loop (block) inside, performs just one step of logic state machines.
    /// Also, internally it prevents recursive entries.
    void mmu_loop();

    /// The main MMU command - select a different slot
    /// @param index of the slot to be selected
    /// @returns false if the operation cannot be performed (Stopped)
    bool tool_change(uint8_t index);

    /// Handling of special Tx, Tc, T? commands
    bool tool_change(const char *special);

    /// Unload of filament in collaboration with the MMU.
    /// That includes rotating the printer's extruder in order to release filament.
    /// @returns false if the operation cannot be performed (Stopped or cold extruder)
    bool unload();

    /// Load (insert) filament just into the MMU (not into printer's nozzle)
    /// @returns false if the operation cannot be performed (Stopped)
    bool load_filament(uint8_t index);

    /// Load (push) filament from the MMU into the printer's nozzle
    /// @returns false if the operation cannot be performed (Stopped or cold extruder)
    bool load_filament_to_nozzle(uint8_t index);

    /// Move MMU's selector aside and push the selected filament forward.
    /// Usable for improving filament's tip or pulling the remaining piece of filament out completely.
    bool eject_filament(uint8_t index, bool recover);

    /// Issue a Cut command into the MMU
    /// Requires unloaded filament from the printer (obviously)
    /// @returns false if the operation cannot be performed (Stopped)
    bool cut_filament(uint8_t index);

    /// @returns the active filament slot index (0-4) or 0xff in case of no active tool
    uint8_t get_current_tool();

    bool set_filament_type(uint8_t index, uint8_t type);

    /// Issue a "button" click into the MMU - to be used from Error screens of the MMU
    /// to select one of the 3 possible options to resolve the issue
    void Button(uint8_t index);

    /// Issue an explicit "homing" command into the MMU
    void Home(uint8_t mode);

    /// @returns current state of FINDA (true=filament present, false=filament not present)
    inline bool FindaDetectsFilament() const { return logic.FindaPressed(); }

private:
    /// Perform software self-reset of the MMU (sends an X0 command)
    void ResetX0();

    /// Trigger reset pin of the MMU
    void TriggerResetPin();

    /// Perform power cycle of the MMU (cold boot)
    /// Please note this is a blocking operation (sleeps for some time inside while doing the power cycle)
    void PowerCycle();

    /// Stop the communication, but keep the MMU powered on (for scenarios with incorrect FW version)
    void StopKeepPowered();

    /// Along with the mmu_loop method, this loops until a response from the MMU is received and acts upon.
    /// In case of an error, it parks the print head and turns off nozzle heating
    void manage_response(const bool move_axes, const bool turn_off_nozzle);

    /// Performs one step of the protocol logic state machine
    /// and reports progress and errors if needed to attached ExtUIs.
    /// Updates the global state of MMU (Active/Connecting/Stopped) at runtime, see @ref State
    StepStatus LogicStep();

    void filament_ramming();
    void execute_extruder_sequence(const E_Step *sequence, int steps);
    void SetActiveExtruder(uint8_t ex);

    /// Reports an error into attached ExtUIs
    /// @param ec error code, see ErrorCode
    void ReportError(ErrorCode ec);

    /// Reports progress of operations into attached ExtUIs
    /// @param pc progress code, see ProgressCode
    void ReportProgress(ProgressCode pc);

    /// Responds to a change of MMU's progress
    /// - plans additional steps, e.g. starts the E-motor after fsensor trigger
    void OnMMUProgressMsg(ProgressCode pc);

    /// Report the msg into the general logging subsystem (through Marlin's SERIAL_ECHO stuff)
    void LogErrorEvent(const char *msg);

    /// Report the msg into the general logging subsystem (through Marlin's SERIAL_ECHO stuff)
    void LogEchoEvent(const char *msg);

    /// Save print and park the print head
    void SaveAndPark(bool move_axes, bool turn_off_nozzle);

    /// Resume print (unpark, turn on heating etc.)
    void ResumeAndUnPark(bool move_axes, bool turn_off_nozzle);

    /// Check for any button/user input coming from the printer's UI
    void CheckUserInput();

    /// Entry check of all external commands.
    /// It can wait until the MMU becomes ready.
    /// Optionally, it can also emit/display an error screen and the user can decide what to do next.
    /// @returns false if the MMU is not ready to perform the command (for whatever reason)
    bool WaitForMMUReady();

    ProtocolLogic logic; ///< implementation of the protocol logic layer
    int extruder;        ///< currently active slot in the MMU ... somewhat... not sure where to get it from yet

    xyz_pos_t resume_position;
    int16_t resume_hotend_temp;

    ProgressCode lastProgressCode = ProgressCode::OK;
    ErrorCode lastErrorCode = ErrorCode::MMU_NOT_RESPONDING;

    StepStatus logicStepLastStatus;

    std::atomic<State_t> state;

    bool mmu_print_saved;
    bool loadFilamentStarted;

    friend struct LoadingToNozzleRAII;
    /// true in case we are doing the LoadToNozzle operation - that means the filament shall be loaded all the way down to the nozzle
    /// unlike the mid-print ToolChange commands, which only load the first ~30mm and then the G-code takes over.
    bool loadingToNozzle;
};

/// following Marlin's way of doing stuff - one and only instance of MMU implementation in the code base
extern MMU2 mmu2;

} // namespace MMU2

#endif