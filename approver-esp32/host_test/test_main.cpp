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
//
// With no arguments it runs everything, which is what a pre-commit check
// wants. With arguments it runs the suites whose name contains one of them —
// `run.cmd i2c pmic` — which is what a debugging loop wants. That exists
// because scrolling past a hundred lines of PASS to find the one that matters
// is how a suite stops being run.

#include <cstring>

#include "fake_platform.h"
#include "unity.h"

void RegisterNavigatorTests(void);
void RegisterSettingsMenuTests(void);
void RegisterTouchCalTests(void);
void RegisterIdleTests(void);
void RegisterClockFaceTests(void);
void RegisterRequestCardTests(void);
void RegisterI2cBusTests(void);
void RegisterPmicTests(void);
void RegisterRtcTests(void);
void RegisterImuTests(void);
void RegisterEs8311Tests(void);
void RegisterConfigTests(void);
void RegisterButtonsTests(void);
void RegisterTimezoneTests(void);
void RegisterSpeakerTests(void);
void RegisterWifiPolicyTests(void);
void RegisterReachabilityTests(void);
void RegisterTimesyncTests(void);
void RegisterNatsTests(void);
void RegisterSigningTests(void);
void RegisterRegistrationTests(void);
void RegisterApprovalTests(void);
void RegisterLimitsTests(void);
void RegisterVectorTests(void);

void setUp(void) { fake::Reset(); }

void tearDown(void) {}

namespace {

int filter_count = 0;
char **filters = nullptr;

bool Wanted(const char *suite) {
    if (filter_count == 0) {
        return true;
    }
    for (int i = 0; i < filter_count; ++i) {
        if (std::strstr(suite, filters[i]) != nullptr) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main(int argc, char **argv) {
    filter_count = argc - 1;
    filters = argv + 1;

    UNITY_BEGIN();

    if (Wanted("navigator")) RegisterNavigatorTests();
    if (Wanted("settings")) RegisterSettingsMenuTests();
    if (Wanted("touch")) RegisterTouchCalTests();
    if (Wanted("clock") || Wanted("face")) RegisterClockFaceTests();
    if (Wanted("request") || Wanted("card")) RegisterRequestCardTests();
    if (Wanted("i2c")) RegisterI2cBusTests();
    if (Wanted("pmic")) RegisterPmicTests();
    if (Wanted("rtc")) RegisterRtcTests();
    if (Wanted("imu")) RegisterImuTests();
    if (Wanted("es8311")) RegisterEs8311Tests();
    if (Wanted("config")) RegisterConfigTests();
    if (Wanted("buttons")) RegisterButtonsTests();
    if (Wanted("timezone")) RegisterTimezoneTests();
    if (Wanted("speaker")) RegisterSpeakerTests();
    if (Wanted("wifi")) RegisterWifiPolicyTests();
    if (Wanted("wifi") || Wanted("reach")) RegisterReachabilityTests();
    if (Wanted("timesync") || Wanted("sync")) RegisterTimesyncTests();
    if (Wanted("nats") || Wanted("bus")) RegisterNatsTests();
    if (Wanted("signing") || Wanted("protocol")) RegisterSigningTests();
    if (Wanted("registration") || Wanted("protocol")) RegisterRegistrationTests();
    if (Wanted("approval") || Wanted("protocol")) RegisterApprovalTests();
    if (Wanted("limits") || Wanted("status")) RegisterLimitsTests();
    if (Wanted("idle") || Wanted("dim")) RegisterIdleTests();
    if (Wanted("vectors") || Wanted("parity") || Wanted("protocol")) RegisterVectorTests();

    return UNITY_END();
}
