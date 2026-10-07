// SPDX-License-Identifier: MIT
#include "support/test_framework.hpp"

#include "minitool/common/source_manager.hpp"

namespace {

using namespace minitool;

TEST(SourceManager, StoresNameAndText) {
    SourceManager sources;
    const FileId id = sources.addFile("main.asm", "MOVI R1, 10\n");
    EXPECT_EQ(sources.name(id), "main.asm");
    EXPECT_EQ(sources.text(id), "MOVI R1, 10\n");
    EXPECT_EQ(sources.fileCount(), 1U);
}

TEST(SourceManager, SplitsLinesAndStripsTerminators) {
    SourceManager sources;
    const FileId id = sources.addFile("a.asm", "one\r\ntwo\nthree");
    EXPECT_EQ(sources.line(id, 1), "one");
    EXPECT_EQ(sources.line(id, 2), "two");
    EXPECT_EQ(sources.line(id, 3), "three");
}

TEST(SourceManager, HandlesTrailingNewlineAndEmptyLines) {
    SourceManager sources;
    const FileId id = sources.addFile("a.asm", "one\n\nthree\n");
    EXPECT_EQ(sources.line(id, 2), "");
    EXPECT_EQ(sources.line(id, 3), "three");
    EXPECT_EQ(sources.line(id, 4), "");  // line after the final newline
}

TEST(SourceManager, RejectsOutOfRangeQueriesInsteadOfCrashing) {
    SourceManager sources;
    const FileId id = sources.addFile("a.asm", "one\n");
    EXPECT_EQ(sources.line(id, 0), "");
    EXPECT_EQ(sources.line(id, 99), "");
    EXPECT_EQ(sources.line(kInvalidFileId, 1), "");
    EXPECT_EQ(sources.name(42), "");
    EXPECT_FALSE(sources.contains(kInvalidFileId));
}

TEST(SourceManager, AssignsDistinctIds) {
    SourceManager sources;
    const FileId a = sources.addFile("a.asm", "a\n");
    const FileId b = sources.addFile("b.asm", "b\n");
    EXPECT_NE(a, b);
    EXPECT_EQ(sources.name(a), "a.asm");
    EXPECT_EQ(sources.name(b), "b.asm");
}

TEST(SourceLocationDefault, IsInvalid) {
    const SourceLocation location;
    EXPECT_FALSE(location.valid());
}

}  // namespace
