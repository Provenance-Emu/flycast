#include <gtest/gtest.h>
#include "stdclass.h"
#include "emulator.h"
#include "cfg/cfg.h"
#include "log/LogManager.h"

// Minimal main function for running tests. This bypasses the full application
// initialization, preventing timeouts during test discovery.
int main(int argc, char **argv) {
    add_system_data_dir("../data/");
    flycast_init(argc, argv);

    // Enable SH4 INFO logs for test diagnostics
    if (LogManager* lm = LogManager::GetInstance())
    {
        lm->SetEnable(LogTypes::SH4, true);
        lm->SetLogLevel(LogTypes::LINFO);
        INFO_LOG(COMMON, "SH4 logging enabled at LINFO level for tests.");
    }

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
