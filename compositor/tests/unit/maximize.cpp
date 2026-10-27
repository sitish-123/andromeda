#include "view/maximize.h"

#include "check.h"

using andromeda::maximizeRequestTargetsEdges;

ANDROMEDA_TEST(freshClientMaximizeTargetsColumn) { CHECK(!maximizeRequestTargetsEdges(false)); }

ANDROMEDA_TEST(clientCanLeaveEdgesMaximize) { CHECK(maximizeRequestTargetsEdges(true)); }

int main() { return RUN_TESTS(); }
