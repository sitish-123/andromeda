#include "view/maximize.h"

#include "check.h"

using umbriel::maximizeRequestTargetsEdges;

UMBRIEL_TEST(freshClientMaximizeTargetsColumn) { CHECK(!maximizeRequestTargetsEdges(false)); }

UMBRIEL_TEST(clientCanLeaveEdgesMaximize) { CHECK(maximizeRequestTargetsEdges(true)); }

int main() { return RUN_TESTS(); }
