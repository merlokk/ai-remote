// The host tier's entry point (CLAUDE.md §10.11).
//
// One binary, one `UNITY_BEGIN`/`UNITY_END`, and a `Register…Tests()` per
// subject — so a new suite is a file and one line here rather than a second
// executable to remember to run.
//
// `setUp` wipes the fake platform before every test. That is deliberately here
// and not in each suite: a test that inherits the previous one's state passes for
// the wrong reason, and the only reliable way to prevent it is for no suite to
// have the option of forgetting.
//
// With no arguments it runs everything, which is what a pre-commit check wants.
// With arguments it runs the suites whose name contains one of them —
// `run.cmd led fido` — which is what a debugging loop wants. That exists because
// scrolling past a hundred lines of PASS to find the one that matters is how a
// suite stops being run.
//
// **Nine of the sibling board's suites are missing and five are new.** Gone with
// the hardware: the navigator, the clock face, the settings menu, the Wi-Fi
// screen, the touch calibration, the idle policy, the I²C bus and the four chips
// on it, and the three web ones. New: the LED's arithmetic, the state ranking,
// CTAPHID framing, CBOR, and CTAP2. What is left in the middle is shared with
// that board and is expected to stay identical.

#include <cstring>

#include "fake_platform.h"
#include "unity.h"

void RegisterRequestCardTests(void);
void RegisterConfigTests(void);
void RegisterButtonsTests(void);
void RegisterTimezoneTests(void);
void RegisterWifiPolicyTests(void);
void RegisterReachabilityTests(void);
void RegisterTimesyncTests(void);
void RegisterNatsTests(void);
void RegisterSigningTests(void);
void RegisterRegistrationTests(void);
void RegisterApprovalTests(void);
void RegisterLimitsTests(void);
void RegisterActivityTests(void);
void RegisterVectorTests(void);

// This board's own.
void RegisterLedTests(void);
void RegisterIndicatorTests(void);
void RegisterCtaphidTests(void);
void RegisterCborTests(void);
void RegisterCtap2Tests(void);

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

    // The two pieces of hardware this board has.
    if (Wanted("led") || Wanted("light")) RegisterLedTests();
    if (Wanted("indicator") || Wanted("led") || Wanted("state")) RegisterIndicatorTests();
    if (Wanted("buttons")) RegisterButtonsTests();

    // The key on the OTG port, bottom up.
    if (Wanted("ctaphid") || Wanted("fido") || Wanted("hid")) RegisterCtaphidTests();
    if (Wanted("cbor") || Wanted("fido")) RegisterCborTests();
    if (Wanted("ctap2") || Wanted("fido") || Wanted("key")) RegisterCtap2Tests();

    // The settings, the zone, and the queue a request waits in.
    if (Wanted("config")) RegisterConfigTests();
    if (Wanted("timezone")) RegisterTimezoneTests();
    if (Wanted("request") || Wanted("card")) RegisterRequestCardTests();

    // The network and the bus.
    if (Wanted("wifi")) RegisterWifiPolicyTests();
    if (Wanted("wifi") || Wanted("reach")) RegisterReachabilityTests();
    if (Wanted("timesync") || Wanted("sync")) RegisterTimesyncTests();
    if (Wanted("nats") || Wanted("bus")) RegisterNatsTests();

    // §7's own bytes, and the two watch-only documents.
    if (Wanted("signing") || Wanted("protocol")) RegisterSigningTests();
    if (Wanted("registration") || Wanted("protocol")) RegisterRegistrationTests();
    if (Wanted("approval") || Wanted("protocol")) RegisterApprovalTests();
    if (Wanted("limits") || Wanted("status")) RegisterLimitsTests();
    if (Wanted("activity") || Wanted("limits") || Wanted("doing")) RegisterActivityTests();

    // Tier 2: the cross-language parity vectors.
    if (Wanted("vectors") || Wanted("parity") || Wanted("protocol")) RegisterVectorTests();

    return UNITY_END();
}
