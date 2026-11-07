// main() of UnrealLibTests: GoogleTest plus the ApprovalTests.cpp listener (which names approved files after the
// running test). Replaces GTest::gtest_main.
//
// Approval settings:
//   * approved files live in an "approved/" directory next to each test source;
//   * a mismatch writes <name>.received.txt beside the approved file and prints a text diff to the console. No GUI
//     diff tool is launched, so CTest and CI runs never block. To accept a change, review the diff and rename the
//     .received.txt file to .approved.txt (or copy it over).

#define APPROVALS_GOOGLETEST_EXISTING_MAIN
#include <ApprovalTests.hpp>

#include <gtest/gtest.h>

#include <memory>

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    ApprovalTests::initializeApprovalTestsForGoogleTests();

    // Both return disposers that restore the previous setting when destroyed: keep them alive for the whole run.
    const auto subdirectory = ApprovalTests::Approvals::useApprovalsSubdirectory("approved");
    const auto reporter =
        ApprovalTests::Approvals::useAsDefaultReporter(std::make_shared<ApprovalTests::TextDiffReporter>());

    return RUN_ALL_TESTS();
}
