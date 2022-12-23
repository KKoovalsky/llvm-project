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

using ::testing::HasSubstr;
using ::testing::StartsWith;

namespace clang {
namespace clangd {
namespace {

TWEAK_TEST(ExtractFunction);

TEST_F(ExtractFunctionTest, FunctionTest) {
  Context = Function;

  // Root statements should have common parent.
  EXPECT_EQ(apply("for(;;) [[1+2; 1+2;]]"), "unavailable");
  // We don't support extraction from lambdas.
  EXPECT_EQ(apply("auto lam = [](){ [[int x;]] }; "), "unavailable");
  // Partial statements aren't extracted.
  EXPECT_THAT(apply("int [[x = 0]];"), "unavailable");
  // FIXME: Support hoisting.
  EXPECT_THAT(apply(" [[int a = 5;]] a++; "), "unavailable");

  // Ensure that end of Zone and Beginning of PostZone being adjacent doesn't
  // lead to break being included in the extraction zone.
  EXPECT_THAT(apply("for(;;) { [[int x;]]break; }"), HasSubstr("extracted"));
  // FIXME: ExtractFunction should be unavailable inside loop construct
  // initializer/condition.
  EXPECT_THAT(apply(" for([[int i = 0;]];);"), HasSubstr("extracted"));
  // Extract certain return
  EXPECT_THAT(apply(" if(true) [[{ return; }]] "), HasSubstr("extracted"));
  // Don't extract uncertain return
  EXPECT_THAT(apply(" if(true) [[if (false) return;]] "),
              StartsWith("unavailable"));
  EXPECT_THAT(
      apply("#define RETURN_IF_ERROR(x) if (x) return\nRETU^RN_IF_ERROR(4);"),
      StartsWith("unavailable"));

  FileName = "a.c";
  EXPECT_THAT(apply(" for([[int i = 0;]];);"), HasSubstr("unavailable"));
}

TEST_F(ExtractFunctionTest, FileTest) {
  // Check all parameters are in order
  std::string ParameterCheckInput = R"cpp(
struct Foo {
  int x;
};
void f(int a) {
  int b;
  int *ptr = &a;
  Foo foo;
  [[a += foo.x + b;
  *ptr++;]]
})cpp";
  std::string ParameterCheckOutput = R"cpp(
struct Foo {
  int x;
};
void extracted(int &a, int &b, int * &ptr, Foo &foo) {
a += foo.x + b;
  *ptr++;
}
void f(int a) {
  int b;
  int *ptr = &a;
  Foo foo;
  extracted(a, b, ptr, foo);
})cpp";
  EXPECT_EQ(apply(ParameterCheckInput), ParameterCheckOutput);

  // Check const qualifier
  std::string ConstCheckInput = R"cpp(
void f(const int c) {
  [[while(c) {}]]
})cpp";
  std::string ConstCheckOutput = R"cpp(
void extracted(const int &c) {
while(c) {}
}
void f(const int c) {
  extracted(c);
})cpp";
  EXPECT_EQ(apply(ConstCheckInput), ConstCheckOutput);

  // Check const qualifier with namespace
  std::string ConstNamespaceCheckInput = R"cpp(
namespace X { struct Y { int z; }; }
int f(const X::Y &y) {
  [[return y.z + y.z;]]
})cpp";
  std::string ConstNamespaceCheckOutput = R"cpp(
namespace X { struct Y { int z; }; }
int extracted(const X::Y &y) {
return y.z + y.z;
}
int f(const X::Y &y) {
  return extracted(y);
})cpp";
  EXPECT_EQ(apply(ConstNamespaceCheckInput), ConstNamespaceCheckOutput);

  // Don't extract when we need to make a function as a parameter.
  EXPECT_THAT(apply("void f() { [[int a; f();]] }"), StartsWith("fail"));

  std::string MethodInput = R"cpp(
    class T {
      void f() {
        [[int x;]]
      }
    };
  )cpp";
  std::string MethodCheckOutput = R"cpp(
    class T {
      void extracted() {
int x;
}
void f() {
        extracted();
      }
    };
  )cpp";
  EXPECT_EQ(apply(MethodInput), MethodCheckOutput);

  std::string OutOfLineMethodInput = R"cpp(
    class T {
      void f();
    };

    void T::f() {
      [[int x;]]
    }
  )cpp";
  std::string OutOfLineMethodCheckOutput = R"cpp(
    class T {
      void extracted();
void f();
    };

    void T::extracted() {
int x;
}
void T::f() {
      extracted();
    }
  )cpp";
  EXPECT_EQ(apply(OutOfLineMethodInput), OutOfLineMethodCheckOutput);

  // We don't extract from templated functions for now as templates are hard
  // to deal with.
  std::string TemplateFailInput = R"cpp(
    template<typename T>
    void f() {
      [[int x;]]
    }
  )cpp";
  EXPECT_EQ(apply(TemplateFailInput), "unavailable");

  std::string MacroInput = R"cpp(
    #define F(BODY) void f() { BODY }
    F ([[int x = 0;]])
  )cpp";
  std::string MacroOutput = R"cpp(
    #define F(BODY) void f() { BODY }
    void extracted() {
int x = 0;
}
F (extracted();)
  )cpp";
  EXPECT_EQ(apply(MacroInput), MacroOutput);

  // Shouldn't crash.
  EXPECT_EQ(apply("void f([[int a]]);"), "unavailable");
  EXPECT_EQ(apply("void f(int a = [[1]]);"), "unavailable");
  // Don't extract if we select the entire function body (CompoundStmt).
  std::string CompoundFailInput = R"cpp(
    void f() [[{
      int a;
    }]]
  )cpp";
  EXPECT_EQ(apply(CompoundFailInput), "unavailable");

  std::string CompoundWithMultipleStatementsFailInput = R"cpp(
    void f() [[{
      int a = 1;
      int b = 2;
      ++b;
      b += a;
    }]]
  )cpp";
  EXPECT_EQ(apply(CompoundWithMultipleStatementsFailInput), "unavailable");
}

TEST_F(ExtractFunctionTest, DifferentHeaderSourceTest) {
  Header = R"cpp(
    class SomeClass {
      void f();
    };
  )cpp";

  std::string OutOfLineSource = R"cpp(
    void SomeClass::f() {
      [[int x;]]
    }
  )cpp";

  std::string OutOfLineSourceOutputCheck = R"cpp(
    void SomeClass::extracted() {
int x;
}
void SomeClass::f() {
      extracted();
    }
  )cpp";

  std::string HeaderOutputCheck = R"cpp(
    class SomeClass {
      void extracted();
void f();
    };
  )cpp";

  llvm::StringMap<std::string> EditedFiles;

  EXPECT_EQ(apply(OutOfLineSource, &EditedFiles), OutOfLineSourceOutputCheck);
  EXPECT_EQ(EditedFiles.begin()->second, HeaderOutputCheck);
}

TEST_F(ExtractFunctionTest, DifferentFilesNestedTest) {
  Header = R"cpp(
    class T {
    class SomeClass {
      void f();
    };
    };
  )cpp";

  std::string NestedOutOfLineSource = R"cpp(
    void T::SomeClass::f() {
      [[int x;]]
    }
  )cpp";

  std::string NestedOutOfLineSourceOutputCheck = R"cpp(
    void T::SomeClass::extracted() {
int x;
}
void T::SomeClass::f() {
      extracted();
    }
  )cpp";

  std::string NestedHeaderOutputCheck = R"cpp(
    class T {
    class SomeClass {
      void extracted();
void f();
    };
    };
  )cpp";

  llvm::StringMap<std::string> EditedFiles;

  EXPECT_EQ(apply(NestedOutOfLineSource, &EditedFiles),
            NestedOutOfLineSourceOutputCheck);
  EXPECT_EQ(EditedFiles.begin()->second, NestedHeaderOutputCheck);
}

TEST_F(ExtractFunctionTest, ConstexprDifferentHeaderSourceTest) {
  Header = R"cpp(
    class SomeClass {
      constexpr void f() const;
    };
  )cpp";

  std::string OutOfLineSource = R"cpp(
    constexpr void SomeClass::f() const {
      [[int x;]]
    }
  )cpp";

  std::string OutOfLineSourceOutputCheck = R"cpp(
    constexpr void SomeClass::extracted() const {
int x;
}
constexpr void SomeClass::f() const {
      extracted();
    }
  )cpp";

  std::string HeaderOutputCheck = R"cpp(
    class SomeClass {
      constexpr void extracted() const;
constexpr void f() const;
    };
  )cpp";

  llvm::StringMap<std::string> EditedFiles;

  EXPECT_EQ(apply(OutOfLineSource, &EditedFiles), OutOfLineSourceOutputCheck);
  EXPECT_NE(EditedFiles.begin(), EditedFiles.end())
      << "The header should be edited and receives the declaration of the new "
         "function";

  if (EditedFiles.begin() != EditedFiles.end()) {
    EXPECT_EQ(EditedFiles.begin()->second, HeaderOutputCheck);
  }
}

TEST_F(ExtractFunctionTest, ConstevalDifferentHeaderSourceTest) {
  ExtraArgs.push_back("--std=c++20");
  Header = R"cpp(
    class SomeClass {
      consteval void f() const;
    };
  )cpp";

  std::string OutOfLineSource = R"cpp(
    consteval void SomeClass::f() const {
      [[int x;]]
    }
  )cpp";

  std::string OutOfLineSourceOutputCheck = R"cpp(
    consteval void SomeClass::extracted() const {
int x;
}
consteval void SomeClass::f() const {
      extracted();
    }
  )cpp";

  std::string HeaderOutputCheck = R"cpp(
    class SomeClass {
      consteval void extracted() const;
consteval void f() const;
    };
  )cpp";

  llvm::StringMap<std::string> EditedFiles;

  EXPECT_EQ(apply(OutOfLineSource, &EditedFiles), OutOfLineSourceOutputCheck);
  EXPECT_NE(EditedFiles.begin(), EditedFiles.end())
      << "The header should be edited and receives the declaration of the new "
         "function";

  if (EditedFiles.begin() != EditedFiles.end()) {
    EXPECT_EQ(EditedFiles.begin()->second, HeaderOutputCheck);
  }
}

TEST_F(ExtractFunctionTest, ConstDifferentHeaderSourceTest) {
  Header = R"cpp(
    class SomeClass {
      void f() const;
    };
  )cpp";

  std::string OutOfLineSource = R"cpp(
    void SomeClass::f() const {
      [[int x;]]
    }
  )cpp";

  std::string OutOfLineSourceOutputCheck = R"cpp(
    void SomeClass::extracted() const {
int x;
}
void SomeClass::f() const {
      extracted();
    }
  )cpp";

  std::string HeaderOutputCheck = R"cpp(
    class SomeClass {
      void extracted() const;
void f() const;
    };
  )cpp";

  llvm::StringMap<std::string> EditedFiles;

  EXPECT_EQ(apply(OutOfLineSource, &EditedFiles), OutOfLineSourceOutputCheck);
  EXPECT_NE(EditedFiles.begin(), EditedFiles.end())
      << "The header should be edited and receives the declaration of the new "
         "function";

  if (EditedFiles.begin() != EditedFiles.end()) {
    EXPECT_EQ(EditedFiles.begin()->second, HeaderOutputCheck);
  }
}

TEST_F(ExtractFunctionTest, StaticDifferentHeaderSourceTest) {
  Header = R"cpp(
    class SomeClass {
      static void f();
    };
  )cpp";

  std::string OutOfLineSource = R"cpp(
    void SomeClass::f() {
      [[int x;]]
    }
  )cpp";

  std::string OutOfLineSourceOutputCheck = R"cpp(
    void SomeClass::extracted() {
int x;
}
void SomeClass::f() {
      extracted();
    }
  )cpp";

  std::string HeaderOutputCheck = R"cpp(
    class SomeClass {
      static void extracted();
static void f();
    };
  )cpp";

  llvm::StringMap<std::string> EditedFiles;

  EXPECT_EQ(apply(OutOfLineSource, &EditedFiles), OutOfLineSourceOutputCheck);
  EXPECT_NE(EditedFiles.begin(), EditedFiles.end())
      << "The header should be edited and receives the declaration of the new "
         "function";

  if (EditedFiles.begin() != EditedFiles.end()) {
    EXPECT_EQ(EditedFiles.begin()->second, HeaderOutputCheck);
  }
}

TEST_F(ExtractFunctionTest, DifferentContextHeaderSourceTest) {
  Header = R"cpp(
    namespace ns{
    class A {
      class C {
      public:
        class RType {};
      };

      class T {
        class SomeClass {
          static C::RType f();
        };
      };
    };
    } // ns
  )cpp";

  std::string OutOfLineSource = R"cpp(
    ns::A::C::RType ns::A::T::SomeClass::f() {
      [[A::C::RType x;
      return x;]]
    }
  )cpp";

  std::string OutOfLineSourceOutputCheck = R"cpp(
    ns::A::C::RType ns::A::T::SomeClass::extracted() {
A::C::RType x;
      return x;
}
ns::A::C::RType ns::A::T::SomeClass::f() {
      return extracted();
    }
  )cpp";

  std::string HeaderOutputCheck = R"cpp(
    namespace ns{
    class A {
      class C {
      public:
        class RType {};
      };

      class T {
        class SomeClass {
          static ns::A::C::RType extracted();
static C::RType f();
        };
      };
    };
    } // ns
  )cpp";

  llvm::StringMap<std::string> EditedFiles;

  EXPECT_EQ(apply(OutOfLineSource, &EditedFiles), OutOfLineSourceOutputCheck);
  EXPECT_EQ(EditedFiles.begin()->second, HeaderOutputCheck);
}

TEST_F(ExtractFunctionTest, DifferentSyntacticContextNamespace) {
  std::string OutOfLineSource = R"cpp(
    namespace ns {
      void f();
    }

    void ns::f() {
      [[int x;]]
    }
  )cpp";

  std::string OutOfLineSourceOutputCheck = R"cpp(
    namespace ns {
      void extracted();
void f();
    }

    void ns::extracted() {
int x;
}
void ns::f() {
      extracted();
    }
  )cpp";

  EXPECT_EQ(apply(OutOfLineSource), OutOfLineSourceOutputCheck);
}

TEST_F(ExtractFunctionTest, ControlFlow) {
  Context = Function;
  // We should be able to extract break/continue with a parent loop/switch.
  EXPECT_THAT(apply(" [[for(;;) if(1) break;]] "), HasSubstr("extracted"));
  EXPECT_THAT(apply(" for(;;) [[while(1) break;]] "), HasSubstr("extracted"));
  EXPECT_THAT(apply(" [[switch(1) { break; }]]"), HasSubstr("extracted"));
  EXPECT_THAT(apply(" [[while(1) switch(1) { continue; }]]"),
              HasSubstr("extracted"));
  // Don't extract break and continue without a loop/switch parent.
  EXPECT_THAT(apply(" for(;;) [[if(1) continue;]] "), StartsWith("fail"));
  EXPECT_THAT(apply(" while(1) [[if(1) break;]] "), StartsWith("fail"));
  EXPECT_THAT(apply(" switch(1) { [[break;]] }"), StartsWith("fail"));
  EXPECT_THAT(apply(" for(;;) { [[while(1) break; break;]] }"),
              StartsWith("fail"));
}

TEST_F(ExtractFunctionTest, ExistingReturnStatement) {
  Context = File;
  const char *Before = R"cpp(
    bool lucky(int N);
    int getNum(bool Superstitious, int Min, int Max) {
      if (Superstitious) [[{
        for (int I = Min; I <= Max; ++I)
          if (lucky(I))
            return I;
        return -1;
      }]] else {
        return (Min + Max) / 2;
      }
    }
  )cpp";
  // FIXME: min/max should be by value.
  // FIXME: avoid emitting redundant braces
  const char *After = R"cpp(
    bool lucky(int N);
    int extracted(int &Min, int &Max) {
{
        for (int I = Min; I <= Max; ++I)
          if (lucky(I))
            return I;
        return -1;
      }
}
int getNum(bool Superstitious, int Min, int Max) {
      if (Superstitious) return extracted(Min, Max); else {
        return (Min + Max) / 2;
      }
    }
  )cpp";
  EXPECT_EQ(apply(Before), After);
}

TEST_F(ExtractFunctionTest, Expressions) {
  std::vector<std::pair<std::string, std::string>> InputOutputs{
      // FULL BINARY EXPRESSIONS
      // Full binary expression, basic maths
      {R"cpp(
void wrapperFun() {
  double a{2.0}, b{3.2}, c{31.55};
  double v{[[b * b - 4 * a * c]]};
}
      )cpp",
       R"cpp(
double extracted(double &a, double &b, double &c) {
return b * b - 4 * a * c;
}
void wrapperFun() {
  double a{2.0}, b{3.2}, c{31.55};
  double v{extracted(a, b, c)};
}
      )cpp"},
      // Full binary expression composed of '+' operator overloads ops
      {
          R"cpp(
struct S {
  S operator+(const S&) {
    return *this;
  }
};
void wrapperFun() {
  S S1, S2, S3;
  auto R{[[S1 + S2 + S3]]};
}
      )cpp",
          R"cpp(
struct S {
  S operator+(const S&) {
    return *this;
  }
};
S extracted(S &S1, S &S2, S &S3) {
return S1 + S2 + S3;
}
void wrapperFun() {
  S S1, S2, S3;
  auto R{extracted(S1, S2, S3)};
}
      )cpp"},
      // Boolean predicate as expression
      {
          R"cpp(
void wrapperFun() {
  int a{1};
  auto R{[[a > 1]]};
}
      )cpp",
          R"cpp(
bool extracted(int &a) {
return a > 1;
}
void wrapperFun() {
  int a{1};
  auto R{extracted(a)};
}
      )cpp"},
      // Expression: captures no global variable
      {R"cpp(
static int a{2};
void wrapperFun() {
  int b{3}, c{31}, d{311};
  auto v{[[a + b + c + d]]};
}
      )cpp",
       R"cpp(
static int a{2};
int extracted(int &b, int &c, int &d) {
return a + b + c + d;
}
void wrapperFun() {
  int b{3}, c{31}, d{311};
  auto v{extracted(b, c, d)};
}
      )cpp"},
      // Full expr: infers return type of call returning by ref
      {
          R"cpp(
struct S {
  S& operator+(const S&) {
    return *this;
  }
};
void wrapperFun() {
  S S1, S2, S3;
  auto R{[[S1 + S2 + S3]]};
}
      )cpp",
          R"cpp(
struct S {
  S& operator+(const S&) {
    return *this;
  }
};
S & extracted(S &S1, S &S2, S &S3) {
return S1 + S2 + S3;
}
void wrapperFun() {
  S S1, S2, S3;
  auto R{extracted(S1, S2, S3)};
}
      )cpp"},
      // Full expr: infers return type of call returning by const-ref
      {
          R"cpp(
struct S {
  const S& operator+(const S&) const {
    return *this;
  }
};
void wrapperFun() {
  S S1, S2, S3;
  auto R{[[S1 + S2 + S3]]};
}
      )cpp",
          R"cpp(
struct S {
  const S& operator+(const S&) const {
    return *this;
  }
};
const S & extracted(S &S1, S &S2, S &S3) {
return S1 + S2 + S3;
}
void wrapperFun() {
  S S1, S2, S3;
  auto R{extracted(S1, S2, S3)};
}
      )cpp"},
      // Captures deeply nested arguments
      {
          R"cpp(
int fw(int a) { return a; };
int add(int a, int b) { return a + b; }
void wrapper() {
    int a{0}, b{1}, c{2}, d{3}, e{4}, f{5};
    int r{[[fw(fw(fw(a))) + fw(fw(add(b, c))) + fw(fw(fw(add(d, e)))) + fw(fw(f))]]};
}
      )cpp",
          R"cpp(
int fw(int a) { return a; };
int add(int a, int b) { return a + b; }
int extracted(int &a, int &b, int &c, int &d, int &e, int &f) {
return fw(fw(fw(a))) + fw(fw(add(b, c))) + fw(fw(fw(add(d, e)))) + fw(fw(f));
}
void wrapper() {
    int a{0}, b{1}, c{2}, d{3}, e{4}, f{5};
    int r{extracted(a, b, c, d, e, f)};
}
      )cpp"},
      // SUBEXPRESSIONS
      // Left-aligned subexpression
      {R"cpp(
void wrapperFun() {
  int a{2}, b{3}, c{31}, d{13};
  auto v{[[a + b]] + c + d};
}
      )cpp",
       R"cpp(
int extracted(int &a, int &b) {
return a + b;
}
void wrapperFun() {
  int a{2}, b{3}, c{31}, d{13};
  auto v{extracted(a, b) + c + d};
}
      )cpp"},
      {R"cpp(
void wrapperFun() {
  int a{2}, b{3}, c{31}, d{13};
  auto v{[[a + b + c]] + d};
}
      )cpp",
       R"cpp(
int extracted(int &a, int &b, int &c) {
return a + b + c;
}
void wrapperFun() {
  int a{2}, b{3}, c{31}, d{13};
  auto v{extracted(a, b, c) + d};
}
      )cpp"},
      // Subexpression from the middle
      {R"cpp(
void wrapperFun() {
  int a{2}, b{3}, c{31}, d{15}, e{300};
  auto v{a + [[b + c + d]] + e};
}
      )cpp",
       R"cpp(
int extracted(int &b, int &c, int &d) {
return b + c + d;
}
void wrapperFun() {
  int a{2}, b{3}, c{31}, d{15}, e{300};
  auto v{a + extracted(b, c, d) + e};
}
      )cpp"},
      // Right-aligned subexpression
      {R"cpp(
void wrapperFun() {
  int a{2}, b{3}, c{31}, d{15}, e{300};
  auto v{a + b + [[c + d + e]]};
}
      )cpp",
       R"cpp(
int extracted(int &c, int &d, int &e) {
return c + d + e;
}
void wrapperFun() {
  int a{2}, b{3}, c{31}, d{15}, e{300};
  auto v{a + b + extracted(c, d, e)};
}
      )cpp"},
      // Larger subexpression from the middle
      {R"cpp(
void wrapperFun() {
  int a{2}, b{3}, c{31}, d{311};
  auto v{a + [[a + b + c + d]] + c};
}
      )cpp",
       R"cpp(
int extracted(int &a, int &b, int &c, int &d) {
return a + b + c + d;
}
void wrapperFun() {
  int a{2}, b{3}, c{31}, d{311};
  auto v{a + extracted(a, b, c, d) + c};
}
      )cpp"},
      // Subexpression with duplicated references
      {R"cpp(
void wrapperFun() {
  int a{2}, b{3}, c{31}, d{311};
  auto v{a + b + [[c + c + c + d + d]] + c};
}
      )cpp",
       R"cpp(
int extracted(int &c, int &d) {
return c + c + c + d + d;
}
void wrapperFun() {
  int a{2}, b{3}, c{31}, d{311};
  auto v{a + b + extracted(c, d) + c};
}
      )cpp"},
      // Subexpression: captures no global variable
      {R"cpp(
static int a{2};
void wrapperFun() {
  int b{3}, c{31}, d{311};
  auto v{[[a + b + c]] + d};
}
      )cpp",
       R"cpp(
static int a{2};
int extracted(int &b, int &c) {
return a + b + c;
}
void wrapperFun() {
  int b{3}, c{31}, d{311};
  auto v{extracted(b, c) + d};
}
      )cpp"},
      // Subexpression: infers return type of call returning by ref, LHS
      {
          R"cpp(
struct LargeStruct {
  char LargeMember[1024];
  LargeStruct& get() {
    return *this;
  }
  LargeStruct operator+(const LargeStruct&) {
    return *this;
  }
};
void wrapperFun() {
  LargeStruct LS1, LS2;
  auto LS3{[[LS1.get()]] + LS2};
}
      )cpp",
          R"cpp(
struct LargeStruct {
  char LargeMember[1024];
  LargeStruct& get() {
    return *this;
  }
  LargeStruct operator+(const LargeStruct&) {
    return *this;
  }
};
LargeStruct & extracted(LargeStruct &LS1) {
return LS1.get();
}
void wrapperFun() {
  LargeStruct LS1, LS2;
  auto LS3{extracted(LS1) + LS2};
}
      )cpp"},
      // Subexpression: infers return type of call returning by ref, most-RHS
      {
          R"cpp(
struct LargeStruct {
  char LargeMember[1024];
  LargeStruct& get() {
    return *this;
  }
  LargeStruct operator+(const LargeStruct&) {
    return *this;
  }
};
void wrapperFun() {
  LargeStruct LS1, LS2, LS3;
  auto LS4{LS1 + LS2 + [[LS3.get()]]};
}
      )cpp",
          R"cpp(
struct LargeStruct {
  char LargeMember[1024];
  LargeStruct& get() {
    return *this;
  }
  LargeStruct operator+(const LargeStruct&) {
    return *this;
  }
};
LargeStruct & extracted(LargeStruct &LS3) {
return LS3.get();
}
void wrapperFun() {
  LargeStruct LS1, LS2, LS3;
  auto LS4{LS1 + LS2 + extracted(LS3)};
}
      )cpp"},
      // Subexpression: infers return type of call returning by ref, middle RHS
      {
          R"cpp(
struct LargeStruct {
  char LargeMember[1024];
  LargeStruct& get() {
    return *this;
  }
  LargeStruct getCopy() {
    return *this;
  }
  LargeStruct operator+(const LargeStruct&) {
    return *this;
  }
};
void wrapperFun() {
  LargeStruct LS1, LS2, LS3;
  auto LS4{LS1.getCopy() + [[LS2.get()]] + LS3};
}
      )cpp",
          R"cpp(
struct LargeStruct {
  char LargeMember[1024];
  LargeStruct& get() {
    return *this;
  }
  LargeStruct getCopy() {
    return *this;
  }
  LargeStruct operator+(const LargeStruct&) {
    return *this;
  }
};
LargeStruct & extracted(LargeStruct &LS2) {
return LS2.get();
}
void wrapperFun() {
  LargeStruct LS1, LS2, LS3;
  auto LS4{LS1.getCopy() + extracted(LS2) + LS3};
}
      )cpp"},
      // Subexpr: infers return type of call returning by const-ref
      {
          R"cpp(
struct LargeStruct {
  char LargeMember[1024];
  const LargeStruct& get() {
    return *this;
  }
  LargeStruct operator+(const LargeStruct&) {
    return *this;
  }
};
void wrapperFun() {
  LargeStruct LS1, LS2;
  auto LS3{LS1 + [[LS2.get()]]};
}
      )cpp",
          R"cpp(
struct LargeStruct {
  char LargeMember[1024];
  const LargeStruct& get() {
    return *this;
  }
  LargeStruct operator+(const LargeStruct&) {
    return *this;
  }
};
const LargeStruct & extracted(LargeStruct &LS2) {
return LS2.get();
}
void wrapperFun() {
  LargeStruct LS1, LS2;
  auto LS3{LS1 + extracted(LS2)};
}
      )cpp"},
      // Subexpression on operator overload, left-aligned
      {
          R"cpp(
struct LargeStruct {
  char LargeMember[1024];
  const LargeStruct& get() {
    return *this;
  }
  LargeStruct& operator+(const LargeStruct&) {
    return *this;
  }
};
void wrapperFun() {
  LargeStruct LS1, LS2, LS3, LS4;
  auto& LS5{[[LS1 + LS2.get()]] + LS3.get() + LS4};
}
      )cpp",
          R"cpp(
struct LargeStruct {
  char LargeMember[1024];
  const LargeStruct& get() {
    return *this;
  }
  LargeStruct& operator+(const LargeStruct&) {
    return *this;
  }
};
LargeStruct & extracted(LargeStruct &LS1, LargeStruct &LS2) {
return LS1 + LS2.get();
}
void wrapperFun() {
  LargeStruct LS1, LS2, LS3, LS4;
  auto& LS5{extracted(LS1, LS2) + LS3.get() + LS4};
}
      )cpp"},
      {
          R"cpp(
struct LargeStruct {
  char LargeMember[1024];
  const LargeStruct& get() {
    return *this;
  }
  LargeStruct& operator+(const LargeStruct&) {
    return *this;
  }
};
void wrapperFun() {
  LargeStruct LS1, LS2, LS3, LS4;
  auto& LS5{[[LS1 + LS2.get() + LS3.get()]] + LS4};
}
      )cpp",
          R"cpp(
struct LargeStruct {
  char LargeMember[1024];
  const LargeStruct& get() {
    return *this;
  }
  LargeStruct& operator+(const LargeStruct&) {
    return *this;
  }
};
LargeStruct & extracted(LargeStruct &LS1, LargeStruct &LS2, LargeStruct &LS3) {
return LS1 + LS2.get() + LS3.get();
}
void wrapperFun() {
  LargeStruct LS1, LS2, LS3, LS4;
  auto& LS5{extracted(LS1, LS2, LS3) + LS4};
}
      )cpp"},
      // Subexpression on operator overload, middle-aligned
      {
          R"cpp(
struct LargeStruct {
  char LargeMember[1024];
  const LargeStruct& get() {
    return *this;
  }
  LargeStruct& operator+(const LargeStruct&) {
    return *this;
  }
};
void wrapperFun() {
  LargeStruct LS1, LS2, LS3, LS4, LS5;
  auto& R{LS1 + [[LS2.get() + LS3 + LS4.get()]] + LS5};
}
      )cpp",
          R"cpp(
struct LargeStruct {
  char LargeMember[1024];
  const LargeStruct& get() {
    return *this;
  }
  LargeStruct& operator+(const LargeStruct&) {
    return *this;
  }
};
LargeStruct & extracted(LargeStruct &LS2, LargeStruct &LS3, LargeStruct &LS4) {
return LS2.get() + LS3 + LS4.get();
}
void wrapperFun() {
  LargeStruct LS1, LS2, LS3, LS4, LS5;
  auto& R{LS1 + extracted(LS2, LS3, LS4) + LS5};
}
      )cpp"},
      // Subexpression on operator overload, right-aligned
      {
          R"cpp(
struct LargeStruct {
  char LargeMember[1024];
  const LargeStruct& get() {
    return *this;
  }
  LargeStruct& operator+(const LargeStruct&) {
    return *this;
  }
};
void wrapperFun() {
  LargeStruct LS1, LS2, LS3, LS4, LS5;
  auto& R{LS1 + LS2.get() + [[LS3 + LS4.get() + LS5]]};
})cpp",
          R"cpp(
struct LargeStruct {
  char LargeMember[1024];
  const LargeStruct& get() {
    return *this;
  }
  LargeStruct& operator+(const LargeStruct&) {
    return *this;
  }
};
LargeStruct & extracted(LargeStruct &LS3, LargeStruct &LS4, LargeStruct &LS5) {
return LS3 + LS4.get() + LS5;
}
void wrapperFun() {
  LargeStruct LS1, LS2, LS3, LS4, LS5;
  auto& R{LS1 + LS2.get() + extracted(LS3, LS4, LS5)};
})cpp"},
      // Boolean predicate as subexpression
      {
          R"cpp(
void wrapperFun() {
  int a{1}, b{2};
  auto R{a > 1 ? [[b <= 0]] : false};
}
      )cpp",
          R"cpp(
bool extracted(int &b) {
return b <= 0;
}
void wrapperFun() {
  int a{1}, b{2};
  auto R{a > 1 ? extracted(b) : false};
}
      )cpp"},
      // Collects deeply nested arguments, left-aligned
      {
          R"cpp(
int fw(int a) { return a; };
int add(int a, int b) { return a + b; }
void wrapper() {
    int a{0}, b{1}, c{2}, d{3}, e{4}, f{5};
    int r{[[fw(fw(fw(a))) + fw(fw(add(b, c))) + fw(fw(fw(add(d, e))))]] + fw(fw(f))};
}
      )cpp",
          R"cpp(
int fw(int a) { return a; };
int add(int a, int b) { return a + b; }
int extracted(int &a, int &b, int &c, int &d, int &e) {
return fw(fw(fw(a))) + fw(fw(add(b, c))) + fw(fw(fw(add(d, e))));
}
void wrapper() {
    int a{0}, b{1}, c{2}, d{3}, e{4}, f{5};
    int r{extracted(a, b, c, d, e) + fw(fw(f))};
}
      )cpp"},
      // Collects deeply nested arguments, middle-aligned
      {
          R"cpp(
int fw(int a) { return a; };
int add(int a, int b) { return a + b; }
void wrapper() {
    int a{0}, b{1}, c{2}, d{3}, e{4}, f{5};
    int r{fw(fw(fw(a))) + [[fw(fw(add(b, c))) + fw(fw(fw(add(d, e))))]] + fw(fw(f))};
}
      )cpp",
          R"cpp(
int fw(int a) { return a; };
int add(int a, int b) { return a + b; }
int extracted(int &b, int &c, int &d, int &e) {
return fw(fw(add(b, c))) + fw(fw(fw(add(d, e))));
}
void wrapper() {
    int a{0}, b{1}, c{2}, d{3}, e{4}, f{5};
    int r{fw(fw(fw(a))) + extracted(b, c, d, e) + fw(fw(f))};
}
      )cpp"},
      // Collects deeply nested arguments, right-aligned
      {
          R"cpp(
int fw(int a) { return a; };
int add(int a, int b) { return a + b; }
void wrapper() {
    int a{0}, b{1}, c{2}, d{3}, e{4}, f{5};
    int r{fw(fw(fw(a))) + [[fw(fw(add(b, c))) + fw(fw(fw(add(d, e)))) + fw(fw(f))]]};
}
      )cpp",
          R"cpp(
int fw(int a) { return a; };
int add(int a, int b) { return a + b; }
int extracted(int &b, int &c, int &d, int &e, int &f) {
return fw(fw(add(b, c))) + fw(fw(fw(add(d, e)))) + fw(fw(f));
}
void wrapper() {
    int a{0}, b{1}, c{2}, d{3}, e{4}, f{5};
    int r{fw(fw(fw(a))) + extracted(b, c, d, e, f)};
}
      )cpp"},
      // FIXME: Support macros: In this case the most-LHS is not omitted!
      {R"cpp(
#define ECHO(X) X
void f() {
    int x = 1 + [[ECHO(2 + 3) + 4]] + 5;
})cpp",
       R"cpp(
#define ECHO(X) X
int extracted() {
return 1 + ECHO(2 + 3) + 4;
}
void f() {
    int x = extracted() + 5;
})cpp"},
  };

  for (const auto &[Input, Output] : InputOutputs) {
    EXPECT_EQ(Output, apply(Input)) << Input;
  }
}

TEST_F(ExtractFunctionTest, ExpressionsInMethodsSingleFile) {
  // TODO: unavailable
  // TODO: available

  std::vector<std::pair<std::string, std::string>> InputOutputs{
      // Expression: Does not capture members as parameters
      // FIXME: If selected area does mutate members, make extracted() const
      {R"cpp(
struct S {
void f() const {
    int a{1}, b{2};
    auto r{[[a + b + mem1 + mem2]]};
}
int mem1{0}, mem2{0};
};
)cpp",
       R"cpp(
struct S {
int extracted(int &a, int &b) const {
return a + b + mem1 + mem2;
}
void f() const {
    int a{1}, b{2};
    auto r{extracted(a, b)};
}
int mem1{0}, mem2{0};
};
)cpp"},
      // Subexpression: Does not capture members as parameters
      {R"cpp(
struct S {
void f() const {
    int a{1}, b{2};
    auto r{a + [[mem1 + mem2 + b + mem1]] + mem2};
}
int mem1{0}, mem2{0};
};
)cpp",
       R"cpp(
struct S {
int extracted(int &b) const {
return mem1 + mem2 + b + mem1;
}
void f() const {
    int a{1}, b{2};
    auto r{a + extracted(b) + mem2};
}
int mem1{0}, mem2{0};
};
)cpp"},
  };

  for (const auto &[Input, Output] : InputOutputs) {
    EXPECT_EQ(Output, apply(Input)) << Input;
  }
}

TEST_F(ExtractFunctionTest, ExpressionInMethodMultiFile) {
  Header = R"cpp(
    class SomeClass {
      void f();
      int mem1{0}, mem2{0};
    };
  )cpp";

  std::string OutOfLineSource = R"cpp(
    void SomeClass::f() {
      int a{1}, b{2};
      int x = [[a + mem1 + b + mem2]];
    }
  )cpp";

  std::string OutOfLineSourceOutputCheck = R"cpp(
    int SomeClass::extracted(int &a, int &b) {
return a + mem1 + b + mem2;
}
void SomeClass::f() {
      int a{1}, b{2};
      int x = extracted(a, b);
    }
  )cpp";

  std::string HeaderOutputCheck = R"cpp(
    class SomeClass {
      int extracted(int &a, int &b);
void f();
      int mem1{0}, mem2{0};
    };
  )cpp";

  llvm::StringMap<std::string> EditedFiles;

  EXPECT_EQ(apply(OutOfLineSource, &EditedFiles), OutOfLineSourceOutputCheck);
  EXPECT_EQ(EditedFiles.begin()->second, HeaderOutputCheck);
}

TEST_F(ExtractFunctionTest, SubexpressionInMethodMultiFile) {
  Header = R"cpp(
    class SomeClass {
      void f();
      int mem1{0}, mem2{0};
    };
  )cpp";

  std::string OutOfLineSource = R"cpp(
    void SomeClass::f() {
      int a{1}, b{2};
      int x = a + [[mem1 + b + mem2]] + mem1;
    }
  )cpp";

  std::string OutOfLineSourceOutputCheck = R"cpp(
    int SomeClass::extracted(int &b) {
return mem1 + b + mem2;
}
void SomeClass::f() {
      int a{1}, b{2};
      int x = a + extracted(b) + mem1;
    }
  )cpp";

  std::string HeaderOutputCheck = R"cpp(
    class SomeClass {
      int extracted(int &b);
void f();
      int mem1{0}, mem2{0};
    };
  )cpp";

  llvm::StringMap<std::string> EditedFiles;

  EXPECT_EQ(apply(OutOfLineSource, &EditedFiles), OutOfLineSourceOutputCheck);
  EXPECT_EQ(EditedFiles.begin()->second, HeaderOutputCheck);
}

} // namespace
} // namespace clangd
} // namespace clang
