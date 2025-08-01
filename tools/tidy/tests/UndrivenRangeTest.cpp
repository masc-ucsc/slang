// SPDX-FileCopyrightText: Michael Popoloski
// SPDX-License-Identifier: MIT

#include "Test.h"
#include "TidyFactory.h"
#include "TidyTest.h"

#include "slang/diagnostics/AnalysisDiags.h"
#include "slang/diagnostics/DiagnosticEngine.h"
#include "slang/diagnostics/TextDiagnosticClient.h"

TEST_CASE("Undriven range: simple case with a two bit bus") {
    std::string output;
    auto result = runCheckTest("UndrivenRange", R"(
module top;
  logic [1:0] a;
  always_comb
    a[0] = 1;
endmodule
)",
                               {}, &output);

    CHECK_FALSE(result);

    CHECK("\n" + output == R"(
source:3:15: warning: [SYNTHESIS-20] variable a has undriven bits: 1
  logic [1:0] a;
              ^
)");
}

TEST_CASE("Undriven range: a 32b bus with missing drivers") {
    std::string output;
    auto result = runCheckTest("UndrivenRange", R"(
module top;
  logic [31:0] a;
  always_comb begin
    a[7:0] = 8'hFF;
    a[11] = 1;
    a[30] = 0;
  end
endmodule
)",
                               {}, &output);

    CHECK_FALSE(result);

    CHECK("\n" + output == R"(
source:3:16: warning: [SYNTHESIS-20] variable a has undriven bits: 8:10, 12:29, 31
  logic [31:0] a;
               ^
)");
}

TEST_CASE("Undriven range: ignore fully undriven variables") {
    std::string output;
    auto result = runCheckTest("UndrivenRange", R"(
module top;
  logic [31:0] a;
endmodule
)",
                               {}, &output);
    CHECK(result);
}
