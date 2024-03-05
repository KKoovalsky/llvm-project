//===-- ExtractFunctionTests.cpp --------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "TweakTesting.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

// using ::testing::HasSubstr;
// using ::testing::StartsWith;

namespace clang {
namespace clangd {
namespace {

TWEAK_TEST(DoDummyStuff);

TEST_F(DoDummyStuffTest, FileTest) {
  const char *Before = R"cpp(
template<typename T> struct Type {
    void bar() {}
};
template<typename T>
Type<T> operator+(const Type<T>&, const Type<T>&)
{ return {}; }
using Alias = Type<char>;
Alias foo() { return {}; }
void baz() {
    auto v{foo() + foo()};
    [[v.bar();
    v.bar();]]
}
  )cpp";

  const char *After = R"cpp(
template<typename T> struct Type {
    void bar() {}
};
template<typename T>
Type<T> operator+(const Type<T>&, const Type<T>&)
{ return {}; }
using Alias = Type<char>;
Alias foo() { return {}; }
void baz() {
    auto v{foo() + foo()};
    v.bar();
    v.bar();
}
  )cpp";
  EXPECT_EQ(apply(Before), After);
  // EXPECT_THAT(apply(" for([[int i = 0;]];);"), HasSubstr("unavailable"));
}
} // namespace
} // namespace clangd
} // namespace clang
