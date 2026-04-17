// Unit tests for the isql-compatible SQL splitter. No database required.

#include <gtest/gtest.h>

#include "llmlog/core/isql_splitter.hpp"

#include <string>
#include <vector>

using llmlog::core::splitIsqlScript;

TEST(IsqlSplitter, EmptyInputYieldsNothing) {
    EXPECT_TRUE(splitIsqlScript("").empty());
    EXPECT_TRUE(splitIsqlScript("   \n  \t ").empty());
    EXPECT_TRUE(splitIsqlScript("-- only a comment\n").empty());
    EXPECT_TRUE(splitIsqlScript("/* only a block comment */").empty());
}

TEST(IsqlSplitter, SingleStatementNoTerminator) {
    auto out = splitIsqlScript("SELECT 1 FROM RDB$DATABASE");
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0], "SELECT 1 FROM RDB$DATABASE");
}

TEST(IsqlSplitter, MultipleSimpleStatements) {
    auto out = splitIsqlScript(
        "CREATE TABLE FOO (X INT);\n"
        "CREATE TABLE BAR (Y INT);\n"
        "CREATE INDEX IDX_FOO ON FOO(X);\n");
    ASSERT_EQ(out.size(), 3u);
    EXPECT_NE(out[0].find("FOO"), std::string::npos);
    EXPECT_NE(out[1].find("BAR"), std::string::npos);
    EXPECT_NE(out[2].find("IDX_FOO"), std::string::npos);
}

TEST(IsqlSplitter, LineCommentsAreStripped) {
    auto out = splitIsqlScript(
        "-- leading comment\n"
        "SELECT 1 FROM RDB$DATABASE; -- trailing comment\n"
        "-- between\n"
        "SELECT 2 FROM RDB$DATABASE;\n");
    ASSERT_EQ(out.size(), 2u);
    EXPECT_NE(out[0].find("1"), std::string::npos);
    EXPECT_NE(out[1].find("2"), std::string::npos);
}

TEST(IsqlSplitter, BlockCommentsAreStripped) {
    auto out = splitIsqlScript(
        "/* header */ SELECT 1 FROM RDB$DATABASE;"
        "/* between */"
        "SELECT 2 FROM RDB$DATABASE;");
    ASSERT_EQ(out.size(), 2u);
}

TEST(IsqlSplitter, SemicolonInsideStringLiteralIsNotTerminator) {
    auto out = splitIsqlScript(
        "INSERT INTO T VALUES ('a;b;c');\n"
        "INSERT INTO T VALUES ('d');\n");
    ASSERT_EQ(out.size(), 2u);
    EXPECT_NE(out[0].find("'a;b;c'"), std::string::npos);
}

TEST(IsqlSplitter, DoubledQuoteEscape) {
    auto out = splitIsqlScript("INSERT INTO T VALUES ('it''s;ok');");
    ASSERT_EQ(out.size(), 1u);
    EXPECT_NE(out[0].find("'it''s;ok'"), std::string::npos);
}

TEST(IsqlSplitter, SetTermSwitchesTerminator) {
    auto out = splitIsqlScript(
        "SET TERM ^ ;\n"
        "CREATE OR ALTER PROCEDURE P1 AS BEGIN EXIT; END ^\n"
        "CREATE OR ALTER PROCEDURE P2 AS BEGIN EXIT; END ^\n"
        "SET TERM ; ^\n"
        "CREATE TABLE T (X INT);\n");
    ASSERT_EQ(out.size(), 3u);
    EXPECT_NE(out[0].find("PROCEDURE P1"), std::string::npos);
    EXPECT_NE(out[1].find("PROCEDURE P2"), std::string::npos);
    EXPECT_NE(out[2].find("CREATE TABLE T"), std::string::npos);
}

TEST(IsqlSplitter, ProcedureBodyWithInternalSemicolonsIsOneStatement) {
    // This is the whole point of SET TERM — inner ';' must NOT be treated
    // as statement boundaries once TERM has been flipped to '^'.
    auto out = splitIsqlScript(
        "SET TERM ^ ;\n"
        "CREATE OR ALTER PROCEDURE COMPLEX AS\n"
        "  DECLARE VARIABLE X INT;\n"
        "BEGIN\n"
        "  X = 1;\n"
        "  X = X + 2;\n"
        "  SUSPEND;\n"
        "END ^\n");
    ASSERT_EQ(out.size(), 1u);
    EXPECT_NE(out[0].find("DECLARE VARIABLE X"), std::string::npos);
    EXPECT_NE(out[0].find("SUSPEND"), std::string::npos);
}
