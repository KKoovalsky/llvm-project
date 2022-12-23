//===--- ExtractFunction.cpp -------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Extracts statements to a new function and replaces the statements with a
// call to the new function.
// Before:
//   void f(int a) {
//     [[if(a < 5)
//       a = 5;]]
//   }
// After:
//   void extracted(int &a) {
//     if(a < 5)
//       a = 5;
//   }
//   void f(int a) {
//     extracted(a);
//   }
//
// - Only extract statements
// - Extracts from non-templated free functions only.
// - Parameters are const only if the declaration was const
//   - Always passed by l-value reference
// - Void return type
// - Cannot extract declarations that will be needed in the original function
//   after extraction.
// - Checks for broken control flow (break/continue without loop/switch)
//
// 1. ExtractFunction is the tweak subclass
//    - Prepare does basic analysis of the selection and is therefore fast.
//      Successful prepare doesn't always mean we can apply the tweak.
//    - Apply does a more detailed analysis and can be slower. In case of
//      failure, we let the user know that we are unable to perform extraction.
// 2. ExtractionZone store information about the range being extracted and the
//    enclosing function.
// 3. NewFunction stores properties of the extracted function and provides
//    methods for rendering it.
// 4. CapturedZoneInfo uses a RecursiveASTVisitor to capture information about
//    the extraction like declarations, existing return statements, etc.
// 5. getExtractedFunction is responsible for analyzing the CapturedZoneInfo and
//    creating a NewFunction.
//===----------------------------------------------------------------------===//

#include "AST.h"
#include "FindTarget.h"
#include "ParsedAST.h"
#include "Selection.h"
#include "SourceCode.h"
#include "refactor/Tweak.h"
#include "support/Logger.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclBase.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/NestedNameSpecifier.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Tooling/Core/Replacement.h"
#include "clang/Tooling/Refactoring/Extract/SourceExtraction.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_os_ostream.h"

#include <algorithm>
#include <optional>

namespace clang {
namespace clangd {
namespace {

using Node = SelectionTree::Node;

// ExtractionZone is the part of code that is being extracted.
// EnclosingFunction is the function/method inside which the zone lies.
// We split the file into 4 parts relative to extraction zone.
enum class ZoneRelative {
  Before,     // Before Zone and inside EnclosingFunction.
  Inside,     // Inside Zone.
  After,      // After Zone and inside EnclosingFunction.
  OutsideFunc // Outside EnclosingFunction.
};

enum FunctionDeclKind {
  InlineDefinition,
  ForwardDeclaration,
  OutOfLineDefinition
};

// Helpers for handling "binary subexpressions" like a + [[b + c]] + d. This is
// taken from ExtractVariable, and adapted a little to handle collection of
// parameters.
struct ExtractedBinarySubexpressionSelection;

class BinarySubexpressionSelection {

public:
  static inline std::optional<BinarySubexpressionSelection>
  tryParse(const SelectionTree::Node &N, const SourceManager *SM) {
    if (const BinaryOperator *Op =
            llvm::dyn_cast_or_null<BinaryOperator>(N.ASTNode.get<Expr>())) {
      return BinarySubexpressionSelection{SM, Op->getOpcode(), Op->getExprLoc(),
                                          N.Children};
    }
    if (const CXXOperatorCallExpr *Op =
            llvm::dyn_cast_or_null<CXXOperatorCallExpr>(
                N.ASTNode.get<Expr>())) {
      if (!Op->isInfixBinaryOp())
        return std::nullopt;

      llvm::SmallVector<const SelectionTree::Node *> SelectedOps;
      // Not all children are args, there's also the callee (operator).
      for (const auto *Child : N.Children) {
        const Expr *E = Child->ASTNode.get<Expr>();
        assert(E && "callee and args should be Exprs!");
        if (E == Op->getArg(0) || E == Op->getArg(1))
          SelectedOps.push_back(Child);
      }
      return BinarySubexpressionSelection{
          SM, BinaryOperator::getOverloadedOpcode(Op->getOperator()),
          Op->getExprLoc(), std::move(SelectedOps)};
    }
    return std::nullopt;
  }

  bool associative() const {
    // Must also be left-associative!
    switch (Kind) {
    case BO_Add:
    case BO_Mul:
    case BO_And:
    case BO_Or:
    case BO_Xor:
    case BO_LAnd:
    case BO_LOr:
      return true;
    default:
      return false;
    }
  }

  bool crossesMacroBoundary() const {
    FileID F = SM->getFileID(ExprLoc);
    for (const SelectionTree::Node *Child : SelectedOperations)
      if (SM->getFileID(Child->ASTNode.get<Expr>()->getExprLoc()) != F)
        return true;
    return false;
  }

  bool isExtractable() const {
    return associative() and not crossesMacroBoundary();
  }

  void dumpSelectedOperations(llvm::raw_ostream &Os,
                              const ASTContext &Cont) const {
    for (const auto *Op : SelectedOperations)
      Op->ASTNode.dump(Os, Cont);
  }

  std::optional<ExtractedBinarySubexpressionSelection> tryExtract() const;

protected:
  struct SelectedOperands {
    llvm::SmallVector<const SelectionTree::Node *> Operands;
    const SelectionTree::Node *Start;
    const SelectionTree::Node *End;
  };

private:
  BinarySubexpressionSelection(
      const SourceManager *SM, BinaryOperatorKind Kind, SourceLocation ExprLoc,
      llvm::SmallVector<const SelectionTree::Node *> SelectedOps)
      : SM{SM}, Kind(Kind), ExprLoc(ExprLoc),
        SelectedOperations(std::move(SelectedOps)) {}

  SelectedOperands getSelectedOperands() const {
    auto [Start, End]{getClosedRangeWithSelectedOperations()};

    llvm::SmallVector<const SelectionTree::Node *> Operands;
    Operands.reserve(SelectedOperations.size());
    const SelectionTree::Node *BinOpSelectionIt{Start->Parent};

    // Edge case: the selection starts from the most-left LHS, e.g. [[a+b+c]]+d
    if (BinOpSelectionIt->Children.size() == 2)
      Operands.emplace_back(BinOpSelectionIt->Children.front()); // LHS
    // In case of operator+ call, the Children will contain the calle as well.
    else if (BinOpSelectionIt->Children.size() == 3)
      Operands.emplace_back(BinOpSelectionIt->Children[1]); // LHS

    // Go up the Binary Operation three, up to the most-right RHS
    for (; BinOpSelectionIt->Children.back() != End;
         BinOpSelectionIt = BinOpSelectionIt->Parent)
      Operands.emplace_back(BinOpSelectionIt->Children.back()); // RHS
    // Remember to add the most-right RHS
    Operands.emplace_back(End);

    SelectedOperands Ops;
    Ops.Start = Start;
    Ops.End = End;
    Ops.Operands = std::move(Operands);
    return Ops;
  }

  std::pair<const SelectionTree::Node *, const SelectionTree::Node *>
  getClosedRangeWithSelectedOperations() const {
    BinaryOperatorKind OuterOp = Kind;
    // Because the tree we're interested in contains only one operator type, and
    // all eligible operators are left-associative, the shape of the tree is
    // very restricted: it's a linked list along the left edges.
    // This simplifies our implementation.
    const SelectionTree::Node *Start = SelectedOperations.front(); // LHS
    const SelectionTree::Node *End = SelectedOperations.back();    // RHS

    // End is already correct: it can't be an OuterOp (as it's
    // left-associative). Start needs to be pushed down int the subtree to the
    // right spot.
    while (true) {
      auto MaybeOp{tryParse(Start->ignoreImplicit(), SM)};
      if (not MaybeOp)
        break;
      const auto &Op{*MaybeOp};
      if (Op.Kind != OuterOp or Op.crossesMacroBoundary())
        break;
      assert(!Op.SelectedOperations.empty() &&
             "got only operator on one side!");
      if (Op.SelectedOperations.size() == 1) { // Only Op.RHS selected
        Start = Op.SelectedOperations.back();
        break;
      }
      // Op.LHS is (at least partially) selected, so descend into it.
      Start = Op.SelectedOperations.front();
    }
    return {Start, End};
  }

protected:
  const SourceManager *SM;
  BinaryOperatorKind Kind;
  SourceLocation ExprLoc;
  // May also contain partially selected operations,
  // e.g. a + [[b + c]], will keep (a + b) BinaryOperator.
  llvm::SmallVector<const SelectionTree::Node *> SelectedOperations;
};

struct ExtractedBinarySubexpressionSelection : BinarySubexpressionSelection {
  ExtractedBinarySubexpressionSelection(BinarySubexpressionSelection BinSubexpr,
                                        SelectedOperands SelectedOps)
      : BinarySubexpressionSelection::BinarySubexpressionSelection(
            std::move(BinSubexpr)),
        Operands{std::move(SelectedOps)} {}

  SourceRange getRange(const LangOptions &LangOpts) const {
    auto MakeHalfOpenFileRange{[&](const SelectionTree::Node *N) {
      return toHalfOpenFileRange(*SM, LangOpts, N->ASTNode.getSourceRange());
    }};

    return SourceRange(MakeHalfOpenFileRange(Operands.Start)->getBegin(),
                       MakeHalfOpenFileRange(Operands.End)->getEnd());
  }

  void dumpSelectedOperands(llvm::raw_ostream &Os,
                            const ASTContext &Cont) const {
    for (const auto *Op : Operands.Operands)
      Op->ASTNode.dump(Os, Cont);
  }

  llvm::SmallVector<const DeclRefExpr *>
  collectReferences(ASTContext &Cont) const {
    llvm::SmallVector<const DeclRefExpr *> Refs;
    auto Matcher{
        ast_matchers::findAll(ast_matchers::declRefExpr().bind("ref"))};
    for (const auto *SelNode : Operands.Operands) {
      auto Matches{ast_matchers::match(Matcher, SelNode->ASTNode, Cont)};
      for (const auto &Match : Matches)
        if (const DeclRefExpr * Ref{Match.getNodeAs<DeclRefExpr>("ref")}; Ref)
          Refs.push_back(Ref);
    }
    return Refs;
  }

private:
  SelectedOperands Operands;
};

std::optional<ExtractedBinarySubexpressionSelection>
BinarySubexpressionSelection::tryExtract() const {
  if (not isExtractable())
    return std::nullopt;
  return ExtractedBinarySubexpressionSelection{*this, getSelectedOperands()};
}

// A RootStmt is a statement that's fully selected including all it's children
// and it's parent is unselected.
// Check if a node is a root statement.
bool isRootStmt(const Node *N) {
  if (!N->ASTNode.get<Stmt>())
    return false;
  // Root statement cannot be partially selected.
  if (N->Selected == SelectionTree::Partial)
    return false;
  // Only DeclStmt can be an unselected RootStmt since VarDecls claim the entire
  // selection range in selectionTree.
  if (N->Selected == SelectionTree::Unselected && !N->ASTNode.get<DeclStmt>())
    return false;
  return true;
}

// Returns the (unselected) parent of all RootStmts given the commonAncestor.
// Returns null if:
// 1. any node is partially selected
// 2. If all completely selected nodes don't have the same common parent
// 3. Any child of Parent isn't a RootStmt.
// Returns null if any child is not a RootStmt.
// We only support extraction of RootStmts since it allows us to extract without
// having to change the selection range. Also, this means that any scope that
// begins in selection range, ends in selection range and any scope that begins
// outside the selection range, ends outside as well.
const Node *getParentOfRootStmts(const Node *CommonAnc) {
  if (!CommonAnc)
    return nullptr;
  const Node *Parent = nullptr;
  switch (CommonAnc->Selected) {
  case SelectionTree::Selection::Unselected:
    // Workaround for an operator call: BinaryOperator will be selecteded
    // completely, but the operator call would be unselected, thus we treat it
    // as it would be completely selected.
    if (CommonAnc->ASTNode.get<CXXOperatorCallExpr>() != nullptr)
      return CommonAnc->Parent;
    // Typically a block, with the { and } unselected, could also be ForStmt etc
    // Ensure all Children are RootStmts.
    Parent = CommonAnc;
    break;
  case SelectionTree::Selection::Partial:
    // Only a fully-selected single statement can be selected.
    return nullptr;
  case SelectionTree::Selection::Complete:
    // If the Common Ancestor is completely selected, then it's a root statement
    // and its parent will be unselected.
    Parent = CommonAnc->Parent;
    // If parent is a DeclStmt, even though it's unselected, we consider it a
    // root statement and return its parent. This is done because the VarDecls
    // claim the entire selection range of the Declaration and DeclStmt is
    // always unselected.
    if (Parent->ASTNode.get<DeclStmt>())
      Parent = Parent->Parent;
    break;
  }
  // Ensure all Children are RootStmts.
  return llvm::all_of(Parent->Children, isRootStmt) ? Parent : nullptr;
}

// The ExtractionZone class forms a view of the code wrt Zone.
struct ExtractionZone {
  const Node *CommonAncestor;
  // Parent of RootStatements being extracted.
  const Node *Parent = nullptr;
  // The half-open file range of the code being extracted.
  SourceRange ZoneRange;
  // The function inside which our zone resides.
  const FunctionDecl *EnclosingFunction = nullptr;
  // The half-open file range of the enclosing function.
  SourceRange EnclosingFuncRange;
  // Set of statements that form the ExtractionZone.
  llvm::DenseSet<const Stmt *> RootStmts;
  // If the extraction zone is a "binary subexpression", then this will be set.
  std::optional<BinarySubexpressionSelection> MaybeBinarySubexpr;

  SourceLocation getInsertionPoint() const {
    return EnclosingFuncRange.getBegin();
  }
  bool isRootStmt(const Stmt *S) const;
  // The last root statement is important to decide where we need to insert a
  // semicolon after the extraction.
  const Node *getLastRootStmt() const { return Parent->Children.back(); }

  // Checks if declarations inside extraction zone are accessed afterwards.
  //
  // This performs a partial AST traversal proportional to the size of the
  // enclosing function, so it is possibly expensive.
  bool requiresHoisting(const SourceManager &SM,
                        const HeuristicResolver *Resolver) const {
    // First find all the declarations that happened inside extraction zone.
    llvm::SmallSet<const Decl *, 1> DeclsInExtZone;
    for (auto *RootStmt : RootStmts) {
      findExplicitReferences(
          RootStmt,
          [&DeclsInExtZone](const ReferenceLoc &Loc) {
            if (!Loc.IsDecl)
              return;
            DeclsInExtZone.insert(Loc.Targets.front());
          },
          Resolver);
    }
    // Early exit without performing expensive traversal below.
    if (DeclsInExtZone.empty())
      return false;
    // Then make sure they are not used outside the zone.
    for (const auto *S : EnclosingFunction->getBody()->children()) {
      if (SM.isBeforeInTranslationUnit(S->getSourceRange().getEnd(),
                                       ZoneRange.getEnd()))
        continue;
      bool HasPostUse = false;
      findExplicitReferences(
          S,
          [&](const ReferenceLoc &Loc) {
            if (HasPostUse ||
                SM.isBeforeInTranslationUnit(Loc.NameLoc, ZoneRange.getEnd()))
              return;
            HasPostUse = llvm::any_of(Loc.Targets,
                                      [&DeclsInExtZone](const Decl *Target) {
                                        return DeclsInExtZone.contains(Target);
                                      });
          },
          Resolver);
      if (HasPostUse)
        return true;
    }
    return false;
  }
};

// Whether the code in the extraction zone is guaranteed to return, assuming
// no broken control flow (unbound break/continue).
// This is a very naive check (does it end with a return stmt).
// Doing some rudimentary control flow analysis would cover more cases.
bool alwaysReturns(const ExtractionZone &EZ) {
  const Stmt *Last = EZ.getLastRootStmt()->ASTNode.get<Stmt>();
  // Unwrap enclosing (unconditional) compound statement.
  while (const auto *CS = llvm::dyn_cast<CompoundStmt>(Last)) {
    if (CS->body_empty())
      return false;
    Last = CS->body_back();
  }
  return llvm::isa<ReturnStmt>(Last);
}

bool ExtractionZone::isRootStmt(const Stmt *S) const {
  return RootStmts.contains(S);
}

// Finds the function in which the zone lies.
const FunctionDecl *findEnclosingFunction(const Node *CommonAnc) {
  // Walk up the SelectionTree until we find a function Decl
  for (const Node *CurNode = CommonAnc; CurNode; CurNode = CurNode->Parent) {
    // Don't extract from lambdas
    if (CurNode->ASTNode.get<LambdaExpr>())
      return nullptr;
    if (const FunctionDecl *Func = CurNode->ASTNode.get<FunctionDecl>()) {
      // FIXME: Support extraction from templated functions.
      if (Func->isTemplated())
        return nullptr;
      if (!Func->getBody())
        return nullptr;
      for (const auto *S : Func->getBody()->children()) {
        // During apply phase, we perform semantic analysis (e.g. figure out
        // what variables requires hoisting). We cannot perform those when the
        // body has invalid statements, so fail up front.
        if (!S)
          return nullptr;
      }
      return Func;
    }
  }
  return nullptr;
}

// Zone Range is the union of SourceRanges of all child Nodes in Parent since
// all child Nodes are RootStmts
llvm::Optional<SourceRange> findZoneRange(const Node *Parent,
                                          const SourceManager &SM,
                                          const LangOptions &LangOpts) {
  SourceRange SR;
  if (auto BeginFileRange = toHalfOpenFileRange(
          SM, LangOpts, Parent->Children.front()->ASTNode.getSourceRange()))
    SR.setBegin(BeginFileRange->getBegin());
  else
    return std::nullopt;
  if (auto EndFileRange = toHalfOpenFileRange(
          SM, LangOpts, Parent->Children.back()->ASTNode.getSourceRange()))
    SR.setEnd(EndFileRange->getEnd());
  else
    return std::nullopt;
  return SR;
}

// Compute the range spanned by the enclosing function.
// FIXME: check if EnclosingFunction has any attributes as the AST doesn't
// always store the source range of the attributes and thus we end up extracting
// between the attributes and the EnclosingFunction.
llvm::Optional<SourceRange>
computeEnclosingFuncRange(const FunctionDecl *EnclosingFunction,
                          const SourceManager &SM,
                          const LangOptions &LangOpts) {
  return toHalfOpenFileRange(SM, LangOpts, EnclosingFunction->getSourceRange());
}

bool isEntireFunctionBodySelected(const ExtractionZone &ExtZone) {
  assert(ExtZone.EnclosingFunction->hasBody() &&
         "We should always be extracting from a function body.");
  return ExtZone.Parent->Children.size() == 1 &&
         ExtZone.getLastRootStmt()->ASTNode.get<Stmt>() ==
             ExtZone.EnclosingFunction->getBody();
}

// FIXME: Check we're not extracting from the initializer/condition of a control
// flow structure.
llvm::Optional<ExtractionZone> findExtractionZone(const Node *CommonAnc,
                                                  const SourceManager &SM,
                                                  const LangOptions &LangOpts) {
  ExtractionZone ExtZone;
  ExtZone.CommonAncestor = CommonAnc;
  auto MaybeBinarySubexpr{
      BinarySubexpressionSelection::tryParse(CommonAnc->ignoreImplicit(), &SM)};
  if (MaybeBinarySubexpr) {
    // FIXME: We shall not allow the user to extract expressions which we don't
    // support, or which are weirdly selected (e.g. a [[+ b + c]]). If the
    // selected subexpression is an entire expression (not only a part of
    // expression), then we don't need the BinarySubexpressionSelection.
    if (const auto &BinarySubexpr{*MaybeBinarySubexpr};
        BinarySubexpr.isExtractable()) {
      ExtZone.MaybeBinarySubexpr = std::move(MaybeBinarySubexpr);
    }
  }
  ExtZone.Parent = getParentOfRootStmts(CommonAnc);
  if (!ExtZone.Parent || ExtZone.Parent->Children.empty())
    return std::nullopt;
  ExtZone.EnclosingFunction = findEnclosingFunction(ExtZone.Parent);
  if (!ExtZone.EnclosingFunction)
    return std::nullopt;
  // Extracting the body of EnclosingFunc would remove it's definition.
  if (isEntireFunctionBodySelected(ExtZone))
    return std::nullopt;
  if (auto FuncRange =
          computeEnclosingFuncRange(ExtZone.EnclosingFunction, SM, LangOpts))
    ExtZone.EnclosingFuncRange = *FuncRange;
  if (auto ZoneRange = findZoneRange(ExtZone.Parent, SM, LangOpts))
    ExtZone.ZoneRange = *ZoneRange;
  if (ExtZone.EnclosingFuncRange.isInvalid() || ExtZone.ZoneRange.isInvalid())
    return std::nullopt;

  for (const Node *Child : ExtZone.Parent->Children)
    ExtZone.RootStmts.insert(Child->ASTNode.get<Stmt>());

  return ExtZone;
}

// Stores information about the extracted function and provides methods for
// rendering it.
struct NewFunction {
  struct Parameter {
    std::string Name;
    QualType TypeInfo;
    bool PassByReference;
    unsigned OrderPriority; // Lower value parameters are preferred first.
    std::string render(const DeclContext *Context) const;
    bool operator<(const Parameter &Other) const {
      return OrderPriority < Other.OrderPriority;
    }
  };
  std::string Name = "extracted";
  QualType ReturnType;
  std::vector<Parameter> Parameters;
  SourceRange BodyRange;
  SourceLocation DefinitionPoint;
  llvm::Optional<SourceLocation> ForwardDeclarationPoint;
  const CXXRecordDecl *EnclosingClass = nullptr;
  const NestedNameSpecifier *DefinitionQualifier = nullptr;
  const DeclContext *SemanticDC = nullptr;
  const DeclContext *SyntacticDC = nullptr;
  const DeclContext *ForwardDeclarationSyntacticDC = nullptr;
  bool CallerReturnsValue = false;
  bool Static = false;
  ConstexprSpecKind Constexpr = ConstexprSpecKind::Unspecified;
  bool Const = false;
  bool Expression = false;

  // Decides whether the extracted function body and the function call need a
  // semicolon after extraction.
  tooling::ExtractionSemicolonPolicy SemicolonPolicy;
  const LangOptions *LangOpts;
  NewFunction(tooling::ExtractionSemicolonPolicy SemicolonPolicy,
              const LangOptions *LangOpts)
      : SemicolonPolicy(SemicolonPolicy), LangOpts(LangOpts) {}
  // Render the call for this function.
  std::string renderCall() const;
  // Render the definition for this function.
  std::string renderDeclaration(FunctionDeclKind K,
                                const DeclContext &SemanticDC,
                                const DeclContext &SyntacticDC,
                                const SourceManager &SM) const;

private:
  std::string
  renderParametersForDeclaration(const DeclContext &Enclosing) const;
  std::string renderParametersForCall() const;
  std::string renderSpecifiers(FunctionDeclKind K) const;
  std::string renderQualifiers() const;
  std::string renderDeclarationName(FunctionDeclKind K) const;
  // Generate the function body.
  std::string getFuncBody(const SourceManager &SM) const;
};

std::string NewFunction::renderParametersForDeclaration(
    const DeclContext &Enclosing) const {
  std::string Result;
  bool NeedCommaBefore = false;
  for (const Parameter &P : Parameters) {
    if (NeedCommaBefore)
      Result += ", ";
    NeedCommaBefore = true;
    Result += P.render(&Enclosing);
  }
  return Result;
}

std::string NewFunction::renderParametersForCall() const {
  std::string Result;
  bool NeedCommaBefore = false;
  for (const Parameter &P : Parameters) {
    if (NeedCommaBefore)
      Result += ", ";
    NeedCommaBefore = true;
    Result += P.Name;
  }
  return Result;
}

std::string NewFunction::renderSpecifiers(FunctionDeclKind K) const {
  std::string Attributes;

  if (Static && K != FunctionDeclKind::OutOfLineDefinition) {
    Attributes += "static ";
  }

  switch (Constexpr) {
  case ConstexprSpecKind::Unspecified:
  case ConstexprSpecKind::Constinit:
    break;
  case ConstexprSpecKind::Constexpr:
    Attributes += "constexpr ";
    break;
  case ConstexprSpecKind::Consteval:
    Attributes += "consteval ";
    break;
  }

  return Attributes;
}

std::string NewFunction::renderQualifiers() const {
  std::string Attributes;

  if (Const) {
    Attributes += " const";
  }

  return Attributes;
}

std::string NewFunction::renderDeclarationName(FunctionDeclKind K) const {
  if (DefinitionQualifier == nullptr || K != OutOfLineDefinition) {
    return Name;
  }

  std::string QualifierName;
  llvm::raw_string_ostream Oss(QualifierName);
  DefinitionQualifier->print(Oss, *LangOpts);
  return llvm::formatv("{0}{1}", QualifierName, Name);
}

std::string NewFunction::renderCall() const {
  return std::string(
      llvm::formatv("{0}{1}({2}){3}", CallerReturnsValue ? "return " : "", Name,
                    renderParametersForCall(),
                    (SemicolonPolicy.isNeededInOriginalFunction() ? ";" : "")));
}

std::string NewFunction::renderDeclaration(FunctionDeclKind K,
                                           const DeclContext &SemanticDC,
                                           const DeclContext &SyntacticDC,
                                           const SourceManager &SM) const {
  std::string Declaration = std::string(llvm::formatv(
      "{0}{1} {2}({3}){4}", renderSpecifiers(K),
      printType(ReturnType, SyntacticDC), renderDeclarationName(K),
      renderParametersForDeclaration(SemanticDC), renderQualifiers()));

  switch (K) {
  case ForwardDeclaration:
    return std::string(llvm::formatv("{0};\n", Declaration));
  case OutOfLineDefinition:
  case InlineDefinition:
    return std::string(
        llvm::formatv("{0} {\n{1}\n}\n", Declaration, getFuncBody(SM)));
    break;
  }
  llvm_unreachable("Unsupported FunctionDeclKind enum");
}

std::string NewFunction::getFuncBody(const SourceManager &SM) const {
  // FIXME: Generate tooling::Replacements instead of std::string to
  // - hoist decls
  // - add return statement
  // - Add semicolon
  auto NewBody{toSourceCode(SM, BodyRange).str() +
               (SemicolonPolicy.isNeededInExtractedFunction() ? ";" : "")};
  if (Expression)
    return "return " + NewBody;
  return NewBody;
}

std::string NewFunction::Parameter::render(const DeclContext *Context) const {
  return printType(TypeInfo, *Context) + (PassByReference ? " &" : " ") + Name;
}

// Stores captured information about Extraction Zone.
struct CapturedZoneInfo {
  struct DeclInformation {
    const Decl *TheDecl;
    ZoneRelative DeclaredIn;
    // index of the declaration or first reference.
    unsigned DeclIndex;
    bool IsReferencedInZone = false;
    bool IsReferencedInPostZone = false;
    // FIXME: Capture mutation information
    DeclInformation(const Decl *TheDecl, ZoneRelative DeclaredIn,
                    unsigned DeclIndex)
        : TheDecl(TheDecl), DeclaredIn(DeclaredIn), DeclIndex(DeclIndex){};
    // Marks the occurence of a reference for this declaration
    void markOccurence(ZoneRelative ReferenceLoc);
  };
  // Maps Decls to their DeclInfo
  llvm::DenseMap<const Decl *, DeclInformation> DeclInfoMap;
  bool HasReturnStmt = false; // Are there any return statements in the zone?
  bool AlwaysReturns = false; // Does the zone always return?
  // Control flow is broken if we are extracting a break/continue without a
  // corresponding parent loop/switch
  bool BrokenControlFlow = false;
  // FIXME: capture TypeAliasDecl and UsingDirectiveDecl
  // FIXME: Capture type information as well.
  DeclInformation *createDeclInfo(const Decl *D, ZoneRelative RelativeLoc);
  DeclInformation *getDeclInfoFor(const Decl *D);
  const DeclInformation *getDeclInfoFor(const Decl *D) const;
};

CapturedZoneInfo::DeclInformation *
CapturedZoneInfo::createDeclInfo(const Decl *D, ZoneRelative RelativeLoc) {
  // The new Decl's index is the size of the map so far.
  auto InsertionResult = DeclInfoMap.insert(
      {D, DeclInformation(D, RelativeLoc, DeclInfoMap.size())});
  // Return the newly created DeclInfo
  return &InsertionResult.first->second;
}

CapturedZoneInfo::DeclInformation *
CapturedZoneInfo::getDeclInfoFor(const Decl *D) {
  auto Iter = DeclInfoMap.find(D);
  if (Iter == DeclInfoMap.end())
    return nullptr;
  return &Iter->second;
}

const CapturedZoneInfo::DeclInformation *
CapturedZoneInfo::getDeclInfoFor(const Decl *D) const {
  auto Iter = DeclInfoMap.find(D);
  if (Iter == DeclInfoMap.end())
    return nullptr;
  return &Iter->second;
}

void CapturedZoneInfo::DeclInformation::markOccurence(
    ZoneRelative ReferenceLoc) {
  switch (ReferenceLoc) {
  case ZoneRelative::Inside:
    IsReferencedInZone = true;
    break;
  case ZoneRelative::After:
    IsReferencedInPostZone = true;
    break;
  default:
    break;
  }
}

bool isLoop(const Stmt *S) {
  return isa<ForStmt>(S) || isa<DoStmt>(S) || isa<WhileStmt>(S) ||
         isa<CXXForRangeStmt>(S);
}

// Captures information from Extraction Zone
CapturedZoneInfo captureZoneInfo(const ExtractionZone &ExtZone) {
  // We use the ASTVisitor instead of using the selection tree since we need to
  // find references in the PostZone as well.
  // FIXME: Check which statements we don't allow to extract.
  class ExtractionZoneVisitor
      : public clang::RecursiveASTVisitor<ExtractionZoneVisitor> {
  public:
    ExtractionZoneVisitor(const ExtractionZone &ExtZone) : ExtZone(ExtZone) {
      TraverseDecl(const_cast<FunctionDecl *>(ExtZone.EnclosingFunction));
    }

    bool TraverseStmt(Stmt *S) {
      if (!S)
        return true;
      bool IsRootStmt = ExtZone.isRootStmt(const_cast<const Stmt *>(S));
      // If we are starting traversal of a RootStmt, we are somewhere inside
      // ExtractionZone
      if (IsRootStmt)
        CurrentLocation = ZoneRelative::Inside;
      addToLoopSwitchCounters(S, 1);
      // Traverse using base class's TraverseStmt
      RecursiveASTVisitor::TraverseStmt(S);
      addToLoopSwitchCounters(S, -1);
      // We set the current location as after since next stmt will either be a
      // RootStmt (handled at the beginning) or after extractionZone
      if (IsRootStmt)
        CurrentLocation = ZoneRelative::After;
      return true;
    }

    // Add Increment to CurNumberOf{Loops,Switch} if statement is
    // {Loop,Switch} and inside Extraction Zone.
    void addToLoopSwitchCounters(Stmt *S, int Increment) {
      if (CurrentLocation != ZoneRelative::Inside)
        return;
      if (isLoop(S))
        CurNumberOfNestedLoops += Increment;
      else if (isa<SwitchStmt>(S))
        CurNumberOfSwitch += Increment;
    }

    bool VisitDecl(Decl *D) {
      Info.createDeclInfo(D, CurrentLocation);
      return true;
    }

    bool VisitDeclRefExpr(DeclRefExpr *DRE) {
      // Find the corresponding Decl and mark it's occurrence.
      const Decl *D = DRE->getDecl();
      auto *DeclInfo = Info.getDeclInfoFor(D);
      // If no Decl was found, the Decl must be outside the enclosingFunc.
      if (!DeclInfo)
        DeclInfo = Info.createDeclInfo(D, ZoneRelative::OutsideFunc);
      DeclInfo->markOccurence(CurrentLocation);
      // FIXME: check if reference mutates the Decl being referred.
      return true;
    }

    bool VisitReturnStmt(ReturnStmt *Return) {
      if (CurrentLocation == ZoneRelative::Inside)
        Info.HasReturnStmt = true;
      return true;
    }

    bool VisitBreakStmt(BreakStmt *Break) {
      // Control flow is broken if break statement is selected without any
      // parent loop or switch statement.
      if (CurrentLocation == ZoneRelative::Inside &&
          !(CurNumberOfNestedLoops || CurNumberOfSwitch))
        Info.BrokenControlFlow = true;
      return true;
    }

    bool VisitContinueStmt(ContinueStmt *Continue) {
      // Control flow is broken if Continue statement is selected without any
      // parent loop
      if (CurrentLocation == ZoneRelative::Inside && !CurNumberOfNestedLoops)
        Info.BrokenControlFlow = true;
      return true;
    }
    CapturedZoneInfo Info;
    const ExtractionZone &ExtZone;
    ZoneRelative CurrentLocation = ZoneRelative::Before;
    // Number of {loop,switch} statements that are currently in the traversal
    // stack inside Extraction Zone. Used to check for broken control flow.
    unsigned CurNumberOfNestedLoops = 0;
    unsigned CurNumberOfSwitch = 0;
  };
  ExtractionZoneVisitor Visitor(ExtZone);
  CapturedZoneInfo Result = std::move(Visitor.Info);
  Result.AlwaysReturns = alwaysReturns(ExtZone);
  return Result;
}

static const ValueDecl *unpackDeclForParameter(const Decl *D) {
  const ValueDecl *VD = dyn_cast_or_null<ValueDecl>(D);
  // Can't parameterise if the Decl isn't a ValueDecl or is a FunctionDecl
  // (this includes the case of recursive call to EnclosingFunc in Zone).
  if (!VD || isa<FunctionDecl>(D))
    return nullptr;
  return VD;
}

static QualType getParameterTypeInfo(const ValueDecl *VD) {
  // Parameter qualifiers are same as the Decl's qualifiers.
  return VD->getType().getNonReferenceType();
}

using Parameters = std::vector<NewFunction::Parameter>;
using MaybeParameters = std::optional<Parameters>;

// FIXME: Check if the declaration has a local/anonymous type
// Returns actual parameters if able to find the parameters successfully and no
// hoisting needed.
static MaybeParameters
createParamsForNoSubexpr(const CapturedZoneInfo &CapturedInfo) {
  std::vector<NewFunction::Parameter> Params;
  for (const auto &KeyVal : CapturedInfo.DeclInfoMap) {
    const auto &DeclInfo = KeyVal.second;
    // If a Decl was Declared in zone and referenced in post zone, it
    // needs to be hoisted (we bail out in that case).
    // FIXME: Support Decl Hoisting.
    if (DeclInfo.DeclaredIn == ZoneRelative::Inside &&
        DeclInfo.IsReferencedInPostZone)
      return std::nullopt;
    if (!DeclInfo.IsReferencedInZone)
      continue; // no need to pass as parameter, not referenced
    if (DeclInfo.DeclaredIn == ZoneRelative::Inside ||
        DeclInfo.DeclaredIn == ZoneRelative::OutsideFunc)
      continue; // no need to pass as parameter, still accessible.
    const auto *VD{unpackDeclForParameter(DeclInfo.TheDecl)};
    if (VD == nullptr)
      return std::nullopt;
    QualType TypeInfo{getParameterTypeInfo(VD)};
    // FIXME: Need better qualifier checks: check mutated status for
    // Decl(e.g. was it assigned, passed as nonconst argument, etc)
    // FIXME: check if parameter will be a non l-value reference.
    // FIXME: We don't want to always pass variables of types like int,
    // pointers, etc by reference.
    bool IsPassedByReference = true;
    // We use the index of declaration as the ordering priority for parameters.
    Params.push_back({std::string(VD->getName()), TypeInfo, IsPassedByReference,
                      DeclInfo.DeclIndex});
  }
  llvm::sort(Params);
  return Params;
}

static MaybeParameters
createParamsForSubexpr(const CapturedZoneInfo &CapturedInfo,
                       const ExtractedBinarySubexpressionSelection &Subexpr,
                       ASTContext &ASTCont) {
  // We use the the Set here, to avoid duplicates, but since the Set will not
  // care about the order, we need to use a vector to collect the unique
  // references in the order of referencing.
  llvm::SmallVector<const ValueDecl *> RefsAsDecls;
  llvm::DenseSet<const ValueDecl *> UniqueRefsAsDecls;

  for (const auto *Ref : Subexpr.collectReferences(ASTCont)) {
    const auto *D{Ref->getDecl()};
    const auto *VD{unpackDeclForParameter(D)};
    // Only collect the ValueDecl-s.
    if (VD == nullptr)
      continue;
    const auto *DeclInfo{CapturedInfo.getDeclInfoFor(D)};
    if (DeclInfo == nullptr or DeclInfo->DeclaredIn != ZoneRelative::Before)
      continue;
    auto [It, IsNew]{UniqueRefsAsDecls.insert(VD)};
    if (IsNew)
      RefsAsDecls.emplace_back(VD);
  }

  std::vector<NewFunction::Parameter> Params;
  std::transform(std::begin(RefsAsDecls), std::end(RefsAsDecls),
                 std::back_inserter(Params), [](const ValueDecl *VD) {
                   QualType TypeInfo{getParameterTypeInfo(VD)};
                   // FIXME: Need better qualifier checks: check mutated status
                   // for Decl(e.g. was it assigned, passed as nonconst
                   // argument, etc)
                   // FIXME: check if parameter will be a non l-value reference.
                   // FIXME: We don't want to always pass variables of types
                   // like int, pointers, etc by reference.
                   bool IsPassedByRef = true;
                   return NewFunction::Parameter{std::string(VD->getName()),
                                                 TypeInfo, IsPassedByRef, 0};
                 });
  return Params;
}

// Adds parameters to ExtractedFunc.
MaybeParameters createParams(
    const std::optional<ExtractedBinarySubexpressionSelection> &MaybeSubexpr,
    const CapturedZoneInfo &CapturedInfo, ASTContext &ASTCont) {
  if (MaybeSubexpr)
    return createParamsForSubexpr(CapturedInfo, *MaybeSubexpr, ASTCont);
  return createParamsForNoSubexpr(CapturedInfo);
}

// Clangd uses open ranges while ExtractionSemicolonPolicy (in Clang Tooling)
// uses closed ranges. Generates the semicolon policy for the extraction and
// extends the ZoneRange if necessary.
tooling::ExtractionSemicolonPolicy
getSemicolonPolicy(ExtractionZone &ExtZone, const SourceManager &SM,
                   const LangOptions &LangOpts) {
  // Get closed ZoneRange.
  SourceRange FuncBodyRange = {ExtZone.ZoneRange.getBegin(),
                               ExtZone.ZoneRange.getEnd().getLocWithOffset(-1)};
  auto SemicolonPolicy = tooling::ExtractionSemicolonPolicy::compute(
      ExtZone.getLastRootStmt()->ASTNode.get<Stmt>(), FuncBodyRange, SM,
      LangOpts);
  // Update ZoneRange.
  ExtZone.ZoneRange.setEnd(FuncBodyRange.getEnd().getLocWithOffset(1));
  return SemicolonPolicy;
}

// Returns true if the selected code is an expression, false otherwise.
bool isExpression(const ExtractionZone &ExtZone) {
  const auto &Node{*ExtZone.Parent};
  return Node.Children.size() == 1 and
         ExtZone.getLastRootStmt()->ASTNode.get<Expr>() != nullptr;
}

// Generate return type for ExtractedFunc. Return false if unable to do so.
std::optional<QualType>
generateReturnProperties(const ExtractionZone &ExtZone,
                         const CapturedZoneInfo &CapturedInfo) {
  // If the selected code always returns, we preserve those return statements.
  // The return type should be the same as the enclosing function.
  // (Others are possible if there are conversions, but this seems clearest).
  const auto &EnclosingFunc{*ExtZone.EnclosingFunction};
  if (CapturedInfo.HasReturnStmt) {
    // If the return is conditional, neither replacing the code with
    // `extracted()` nor `return extracted()` is correct.
    if (!CapturedInfo.AlwaysReturns)
      return std::nullopt;
    QualType Ret = EnclosingFunc.getReturnType();
    // Once we support members, it'd be nice to support e.g. extracting a
    // method of Foo<T> that returns T. But it's not clear when that's safe.
    if (Ret->isDependentType())
      return std::nullopt;
    return Ret;
  }
  // If the selected code is an expression, then take the return type of it.
  if (const auto &Node{*ExtZone.Parent}; Node.Children.size() == 1) {
    if (const Expr * Expression{ExtZone.getLastRootStmt()->ASTNode.get<Expr>()};
        Expression) {
      if (const auto *Call{llvm::dyn_cast_or_null<CallExpr>(Expression)};
          Call) {
        const auto &ASTCont{ExtZone.EnclosingFunction->getParentASTContext()};
        return Call->getCallReturnType(ASTCont);
      }
      return Expression->getType();
    }
  }
  // FIXME: Generate new return statement if needed.
  return EnclosingFunc.getParentASTContext().VoidTy;
}

void captureMethodInfo(NewFunction &ExtractedFunc,
                       const CXXMethodDecl *Method) {
  ExtractedFunc.Static = Method->isStatic();
  ExtractedFunc.Const = Method->isConst();
  ExtractedFunc.EnclosingClass = Method->getParent();
}

// FIXME: add support for adding other function return types besides void.
// FIXME: assign the value returned by non void extracted function.
llvm::Expected<NewFunction> getExtractedFunction(ExtractionZone &ExtZone,
                                                 const SourceManager &SM,
                                                 const LangOptions &LangOpts) {
  CapturedZoneInfo CapturedInfo = captureZoneInfo(ExtZone);
  // Bail out if any break of continue exists
  if (CapturedInfo.BrokenControlFlow)
    return error("Cannot extract break/continue without corresponding "
                 "loop/switch statement.");
  NewFunction ExtractedFunc(getSemicolonPolicy(ExtZone, SM, LangOpts),
                            &LangOpts);

  ExtractedFunc.SyntacticDC =
      ExtZone.EnclosingFunction->getLexicalDeclContext();
  ExtractedFunc.SemanticDC = ExtZone.EnclosingFunction->getDeclContext();
  ExtractedFunc.DefinitionQualifier = ExtZone.EnclosingFunction->getQualifier();
  ExtractedFunc.Constexpr = ExtZone.EnclosingFunction->getConstexprKind();

  if (const auto *Method =
          llvm::dyn_cast<CXXMethodDecl>(ExtZone.EnclosingFunction))
    captureMethodInfo(ExtractedFunc, Method);

  if (ExtZone.EnclosingFunction->isOutOfLine()) {
    // FIXME: Put the extracted method in a private section if it's a class or
    // maybe in an anonymous namespace
    const auto *FirstOriginalDecl =
        ExtZone.EnclosingFunction->getCanonicalDecl();
    auto DeclPos =
        toHalfOpenFileRange(SM, LangOpts, FirstOriginalDecl->getSourceRange());
    if (!DeclPos)
      return error("Declaration is inside a macro");
    ExtractedFunc.ForwardDeclarationPoint = DeclPos->getBegin();
    ExtractedFunc.ForwardDeclarationSyntacticDC = ExtractedFunc.SemanticDC;
  }

  auto &ASTCont{ExtZone.EnclosingFunction->getASTContext()};
  ExtractedFunc.Expression = isExpression(ExtZone);
  std::optional<ExtractedBinarySubexpressionSelection> MaybeExtractedSubexpr;
  if (ExtZone.MaybeBinarySubexpr) {
    MaybeExtractedSubexpr = ExtZone.MaybeBinarySubexpr->tryExtract();
    ExtractedFunc.BodyRange = MaybeExtractedSubexpr->getRange(LangOpts);
  } else {
    ExtractedFunc.BodyRange = ExtZone.ZoneRange;
  }

  ExtractedFunc.DefinitionPoint = ExtZone.getInsertionPoint();
  ExtractedFunc.CallerReturnsValue = CapturedInfo.AlwaysReturns;

  auto MaybeRetType{generateReturnProperties(ExtZone, CapturedInfo)};
  auto MaybeParams{createParams(MaybeExtractedSubexpr, CapturedInfo, ASTCont)};
  if (not MaybeRetType || not MaybeParams)
    return error("Too complex to extract.");
  ExtractedFunc.ReturnType = std::move(*MaybeRetType);
  ExtractedFunc.Parameters = std::move(*MaybeParams);
  return ExtractedFunc;
}

class ExtractFunction : public Tweak {
public:
  const char *id() const final;
  bool prepare(const Selection &Inputs) override;
  Expected<Effect> apply(const Selection &Inputs) override;
  std::string title() const override { return "Extract to function"; }
  llvm::StringLiteral kind() const override {
    return CodeAction::REFACTOR_KIND;
  }

private:
  ExtractionZone ExtZone;
};

REGISTER_TWEAK(ExtractFunction)
tooling::Replacement replaceWithFuncCall(const NewFunction &ExtractedFunc,
                                         const SourceManager &SM,
                                         const LangOptions &LangOpts) {
  std::string FuncCall = ExtractedFunc.renderCall();
  return tooling::Replacement(
      SM, CharSourceRange(ExtractedFunc.BodyRange, false), FuncCall, LangOpts);
}

tooling::Replacement createFunctionDefinition(const NewFunction &ExtractedFunc,
                                              const SourceManager &SM) {
  FunctionDeclKind DeclKind = InlineDefinition;
  if (ExtractedFunc.ForwardDeclarationPoint)
    DeclKind = OutOfLineDefinition;
  std::string FunctionDef = ExtractedFunc.renderDeclaration(
      DeclKind, *ExtractedFunc.SemanticDC, *ExtractedFunc.SyntacticDC, SM);

  return tooling::Replacement(SM, ExtractedFunc.DefinitionPoint, 0,
                              FunctionDef);
}

tooling::Replacement createForwardDeclaration(const NewFunction &ExtractedFunc,
                                              const SourceManager &SM) {
  std::string FunctionDecl = ExtractedFunc.renderDeclaration(
      ForwardDeclaration, *ExtractedFunc.SemanticDC,
      *ExtractedFunc.ForwardDeclarationSyntacticDC, SM);
  SourceLocation DeclPoint = *ExtractedFunc.ForwardDeclarationPoint;

  return tooling::Replacement(SM, DeclPoint, 0, FunctionDecl);
}

// Returns true if ExtZone contains any ReturnStmts.
bool hasReturnStmt(const ExtractionZone &ExtZone) {
  class ReturnStmtVisitor
      : public clang::RecursiveASTVisitor<ReturnStmtVisitor> {
  public:
    bool VisitReturnStmt(ReturnStmt *Return) {
      Found = true;
      return false; // We found the answer, abort the scan.
    }
    bool Found = false;
  };

  ReturnStmtVisitor V;
  for (const Stmt *RootStmt : ExtZone.RootStmts) {
    V.TraverseStmt(const_cast<Stmt *>(RootStmt));
    if (V.Found)
      break;
  }
  return V.Found;
}

bool ExtractFunction::prepare(const Selection &Inputs) {
  const LangOptions &LangOpts = Inputs.AST->getLangOpts();
  if (!LangOpts.CPlusPlus)
    return false;
  const Node *CommonAnc = Inputs.ASTSelection.commonAncestor();
  const SourceManager &SM = Inputs.AST->getSourceManager();
  auto MaybeExtZone = findExtractionZone(CommonAnc, SM, LangOpts);
  if (!MaybeExtZone ||
      (hasReturnStmt(*MaybeExtZone) && !alwaysReturns(*MaybeExtZone)))
    return false;

  // FIXME: Get rid of this check once we support hoisting.
  if (MaybeExtZone->requiresHoisting(SM, Inputs.AST->getHeuristicResolver()))
    return false;

  ExtZone = std::move(*MaybeExtZone);
  return true;
}

Expected<Tweak::Effect> ExtractFunction::apply(const Selection &Inputs) {
  const SourceManager &SM = Inputs.AST->getSourceManager();
  const LangOptions &LangOpts = Inputs.AST->getLangOpts();
  auto ExtractedFunc = getExtractedFunction(ExtZone, SM, LangOpts);
  // FIXME: Add more types of errors.
  if (!ExtractedFunc)
    return ExtractedFunc.takeError();
  tooling::Replacements Edit;
  if (auto Err = Edit.add(createFunctionDefinition(*ExtractedFunc, SM)))
    return std::move(Err);
  if (auto Err = Edit.add(replaceWithFuncCall(*ExtractedFunc, SM, LangOpts)))
    return std::move(Err);

  if (auto FwdLoc = ExtractedFunc->ForwardDeclarationPoint) {
    // If the fwd-declaration goes in the same file, merge into Replacements.
    // Otherwise it needs to be a separate file edit.
    if (SM.isWrittenInSameFile(ExtractedFunc->DefinitionPoint, *FwdLoc)) {
      if (auto Err = Edit.add(createForwardDeclaration(*ExtractedFunc, SM)))
        return std::move(Err);
    } else {
      auto MultiFileEffect = Effect::mainFileEdit(SM, std::move(Edit));
      if (!MultiFileEffect)
        return MultiFileEffect.takeError();

      tooling::Replacements OtherEdit(
          createForwardDeclaration(*ExtractedFunc, SM));
      if (auto PathAndEdit =
              Tweak::Effect::fileEdit(SM, SM.getFileID(*FwdLoc), OtherEdit))
        MultiFileEffect->ApplyEdits.try_emplace(PathAndEdit->first,
                                                PathAndEdit->second);
      else
        return PathAndEdit.takeError();
      return MultiFileEffect;
    }
  }
  return Effect::mainFileEdit(SM, std::move(Edit));
}

} // namespace
} // namespace clangd
} // namespace clang
