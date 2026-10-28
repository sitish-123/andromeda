#include "view/maximize.h"

#include "check.h"

using andromeda::maximizeRequestTargetsEdges;

ANDROMEDA_TEST(freshClientMaximizeTargetsColumn) { CHECK(!maximizeRequestTargetsEdges(true, false, false)); }

ANDROMEDA_TEST(clientCanLeaveEdgesMaximize) { CHECK(maximizeRequestTargetsEdges(false, true, false)); }

ANDROMEDA_TEST(configuredPolicySendsFreshRequestToEdges) { CHECK(maximizeRequestTargetsEdges(true, false, true)); }

ANDROMEDA_TEST(configuredPolicyUnmaximizeTargetsColumn) { CHECK(!maximizeRequestTargetsEdges(false, false, true)); }

int main() { return RUN_TESTS(); }
