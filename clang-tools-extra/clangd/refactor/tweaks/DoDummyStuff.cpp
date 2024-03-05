#include "refactor/Tweak.h"
#include "support/Logger.h"
#include "clang/AST/Stmt.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"

namespace clang::clangd {

class DoDummyStuff : public Tweak {
public:
  const char *id() const final;

  bool prepare(const Selection &Inputs) override {
    return Inputs.ASTSelection.commonAncestor() != nullptr;
  };

  Expected<Effect> apply(const Selection &Inputs) override;

  std::string title() const override { return "Do dummy stuff"; }

  llvm::StringLiteral kind() const override {
    return CodeAction::REFACTOR_KIND;
  }

private:
};

REGISTER_TWEAK(DoDummyStuff)

Expected<Tweak::Effect> DoDummyStuff::apply(const Tweak::Selection &Inputs) {
  tooling::Replacements Edit;
  const auto &SM{Inputs.AST->getSourceManager()};
  auto &ASTCont{Inputs.AST->getASTContext()};
  const auto *CommonAnc{Inputs.ASTSelection.commonAncestor()};

  std::string S;
  llvm::raw_string_ostream Os{S};
  CommonAnc->ASTNode.dump(Os, ASTCont);

  auto Matcher{
      ast_matchers::findAll(ast_matchers::declStmt().bind("declStmt"))};
  auto Matches{ast_matchers::match(Matcher, CommonAnc->ASTNode, ASTCont)};
  vlog("DoDummyStuff: Matches: {0}", Matches.size());
  for (const auto &Match : Matches) {
    vlog("DoDummyStuff: Begin match:");
    if (const auto *Node{Match.getNodeAs<DeclStmt>("declStmt")};
        Node != nullptr) {
      Node->dump();
      Node->children().begin()->dump();
    }
    vlog("DoDummyStuff: End match!:");
  }
  // CommonAnc->Children
  // Get the child which is a DeclStmt.
  // vlog("DoDummyStuff: {0}, is selected? {1}", S,
  //      static_cast<unsigned>(CommonAnc->Selected));

  auto Loc{CommonAnc->ASTNode.getSourceRange().getBegin()};
  if (auto Err = Edit.add(tooling::Replacement{SM, Loc, 0, ""}))
    return std::move(Err);
  return Effect::mainFileEdit(SM, std::move(Edit));
}

} // namespace clang::clangd
