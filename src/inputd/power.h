#ifndef BRICKOS_INPUTD_POWER_H
#define BRICKOS_INPUTD_POWER_H

#include <stdint.h>

#define INPUTD_POWER_HOLD_MS 1500
#define INPUTD_POWER_DIR "/tmp/brickos"
#define INPUTD_POWER_MARKER INPUTD_POWER_DIR "/power-hold"
#define INPUTD_IO_TEST_MARKER "/tmp/keytestmode"

enum inputd_power_state {
    INPUTD_POWER_IDLE,
    INPUTD_POWER_HELD,
    INPUTD_POWER_SUPPRESSED,
    INPUTD_POWER_MARKER_BUSY,
    INPUTD_POWER_REQUEST_RAISED,
    INPUTD_POWER_PUBLISH_FAILED,
    INPUTD_POWER_SOURCE_DISARMED
};

struct inputd_power_engine {
    uint64_t pressed_at_ms;
    uint64_t sequence;
    int held;
    int armed;
    int suppressed;
    int fired;
    int marker_blocked;
    enum inputd_power_state state;
};

void inputd_power_engine_init(struct inputd_power_engine *engine, int initially_held);
// Returns 1 once when the hold crosses strictly beyond 1500 ms and no request is pending.
// Suppression (IO test or a non-MainUI frontend/game) applies to the complete hold,
// even if the suppressing condition later clears.
int inputd_power_engine_update(struct inputd_power_engine *engine, uint64_t now_ms,
                               int held, int suppress, int marker_present);
enum inputd_game_power_action { INPUTD_GAME_POWER_NONE, INPUTD_GAME_POWER_LONG, INPUTD_GAME_POWER_SHORT };
// Game sessions also classify a released short press. Same suppression/single-fire contract.
enum inputd_game_power_action inputd_power_engine_update_game(struct inputd_power_engine *engine,
                              uint64_t now_ms,int held,int suppress,int request_pending);
// Stable for one stroke and scoped by the full validated session nonce on the channel.
int inputd_power_transaction(const struct inputd_power_engine *engine,const char *session,char transaction[33]);
void inputd_power_engine_publish_result(struct inputd_power_engine *engine, int result);
const char *inputd_power_state_name(enum inputd_power_state state);

// MainUI owns POWER only while MainUI is running. In games/apps stock keymon
// receives the PMIC key and keeps its exit-to-MainUI behavior. If ownership
// returns while POWER is held, wait for release before reclaiming the source.
int inputd_power_should_grab(int io_test, int mainui_active, int held, int currently_grabbed);
// A supported-game press keeps its release with the current owner even if its
// lease disappears mid-stroke; Hardware Test always takes precedence.
int inputd_game_power_should_grab(int io_test,int mainui_active,int game_valid,
                                  int held,int currently_grabbed,int game_press);

// Atomically creates power-hold without replacing a request that osdd has not consumed.
// Returns 1 when created, 0 when an existing marker was left untouched, or -1 on error.
int inputd_power_publish_marker(const char *directory);

#endif
