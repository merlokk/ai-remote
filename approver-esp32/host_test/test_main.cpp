// The host tier's entry point (CLAUDE.md §10.11).
//
// One binary, one `UNITY_BEGIN`/`UNITY_END`, and a `Register…Tests()` per
// subject — so a new suite is a file and one line here rather than a second
// executable to remember to run.
//
// `setUp` wipes the fake platform before every test. That is deliberately here
// and not in each suite: a test that inherits the previous one's transfer log
// passes for the wrong reason, and the only reliable way to prevent it is for
// no suite to have the option of forgetting.

#include "fake_platform.h"
#include "unity.h"

void RegisterNavigatorTests(void);
void RegisterI2cBusTests(void);
void RegisterPmicTests(void);
void RegisterRtcTests(void);
void RegisterImuTests(void);
void RegisterEs8311Tests(void);

void setUp(void) { fake::Reset(); }

void tearDown(void) {}

int main(void) {
    UNITY_BEGIN();

    RegisterNavigatorTests();
    RegisterI2cBusTests();
    RegisterPmicTests();
    RegisterRtcTests();
    RegisterImuTests();
    RegisterEs8311Tests();

    return UNITY_END();
}
