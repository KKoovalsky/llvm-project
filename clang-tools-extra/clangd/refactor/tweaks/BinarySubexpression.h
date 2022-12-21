//===--- BinarySubexpression.h - binary subexpr extraction -------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "Selection.h"
#include "SourceCode.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/OperationKinds.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceLocation.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/raw_ostream.h"

#include <optional>

namespace clang::clangd {

// Helpers for handling "binary subexpressions" like a + [[b + c]] + d.
//
// These are special, because the formal AST doesn't match what users expect:
// - the AST is ((a + b) + c) + d, so the ancestor expression is `a + b + c`.
// - but extracting `b + c` is reasonable, as + is (mathematically) associative.
//
// So we try to support these cases with some restrictions:
//  - the operator must be associative
//  - no mixing of operators is allowed
//  - we don't look inside macro expansions in the subexpressions
//  - we only adjust the extracted range, so references in the unselected parts
//    of the AST expression (e.g. `a`) are still considered referenced for
//    the purposes of calculating the insertion point.
//    FIXME: it would be nice to exclude these references, by micromanaging
//    the computeReferencedDecls() calls around the binary operator tree.
// Information extracted about a binary operator encounted in a SelectionTree.
// It can represent either an overloaded or built-in operator.

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

inline std::optional<ExtractedBinarySubexpressionSelection>
BinarySubexpressionSelection::tryExtract() const {
  if (not isExtractable())
    return std::nullopt;
  return ExtractedBinarySubexpressionSelection{*this, getSelectedOperands()};
}
} // namespace clang::clangd
