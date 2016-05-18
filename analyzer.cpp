#include <stdio.h>
#include <iostream>

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/StmtVisitor.h>
#include <clang/AST/RecursiveASTVisitor.h>

#include "analyzer.h"

using namespace llvm;
using namespace clang;

class MethodBodyChecker : public RecursiveASTVisitor<MethodBodyChecker> {
public:
  MethodBodyChecker(ASTContext &Context, QualType &returnType) : Context(Context), ReturnType(returnType) {

  }

  bool VisitStmt(Stmt *stmt) {
    DeclStmt *decl = llvm::dyn_cast<DeclStmt>(stmt);
    if (decl) {
      DeclGroupRef group = decl->getDeclGroup();
      DeclGroupRef::iterator it;
      for(it = group.begin(); it != group.end(); it++) {
        VarDecl *vd = llvm::dyn_cast<VarDecl>(*it);
        if (vd) {
          QualType varType = vd->getType();

          Expr *init = vd->getInit();
          if (init) {
            QualType valType = this->getType(init);
            if (!this->isNonNullExpr(init)) {
              if (!this->testTypeNullability(varType, valType)) {
                std::cout << "Nullability mismatch!! (var decl:" << vd->getNameAsString() << ")";
                std::cout << std::endl;
                vd->getLocation().dump(vd->getASTContext().getSourceManager());
                std::cout << std::endl;
              }
            }
          }
        }
      }
    }

    ObjCMessageExpr *callExpr = llvm::dyn_cast<ObjCMessageExpr>(stmt);
    if (callExpr) {
      ObjCMethodDecl *decl = callExpr->getMethodDecl();
      if (decl) {
        unsigned index = 0;

        ObjCMethodDecl::param_iterator it;
        for (it = decl->param_begin(); it != decl->param_end(); it++) {
          ParmVarDecl *d = *it;
          QualType paramQType = d->getType();

          Expr *arg = callExpr->getArg(index);
          QualType argType = this->getType(arg);

          if (!this->isNonNullExpr(arg)) {
            if (!this->testTypeNullability(paramQType, argType)) {
                std::cout << "Nullability mismatch!! (method call: " << decl->getNameAsString() << ")";
                std::cout << std::endl;
                arg->getExprLoc().dump(this->Context.getSourceManager());
                std::cout << std::endl;
            }
          }

          index++;
        }
      }
    }

    BinaryOperator *assign = llvm::dyn_cast<BinaryOperator>(stmt);
    if (assign) {
      if (assign->getOpcode() == BO_Assign) {
        Expr *lhs = assign->getLHS();
        Expr *rhs = assign->getRHS();

        if (!this->isNonNullExpr(rhs)) {
          if (!this->testTypeNullability(this->getType(lhs), this->getType(rhs))) {
                std::cout << "Nullability mismatch!! (assignment)";
                std::cout << std::endl;
                rhs->getExprLoc().dump(this->Context.getSourceManager());
                std::cout << std::endl;
          }
        }
      }
    }

    ReturnStmt *retStmt = llvm::dyn_cast<ReturnStmt>(stmt);
    if (retStmt) {
      Expr *value = retStmt->getRetValue();
      if (!this->isNonNullExpr(value)) {
        if (value) {
          QualType type = this->getType(value);
          if (!this->testTypeNullability(ReturnType, type)) {
            std::cout << "Nullability mismatch!! (return)";
            std::cout << std::endl;
            value->getExprLoc().dump(this->Context.getSourceManager());
            std::cout << std::endl;
          }
        }
      }
    }

    return true;
  }

  QualType getType(const Expr *expr) {
    const Expr *e = expr->IgnoreParenImpCasts();

    const PseudoObjectExpr *poe = llvm::dyn_cast<PseudoObjectExpr>(e);
    if (poe) {
      return this->getType(poe->getSyntacticForm());
    }

    const ObjCPropertyRefExpr *propRef = llvm::dyn_cast<ObjCPropertyRefExpr>(e);
    if (propRef) {
      if (propRef->isImplicitProperty()) {
        const ObjCMethodDecl *getter = propRef->getImplicitPropertyGetter();
        return getter->getReturnType();
      }
      if (propRef->isExplicitProperty()) {
        ObjCPropertyDecl *decl = propRef->getExplicitProperty();
        return decl->getType();
      }
    }

    return e->getType();
  }

  bool testTypeNullability(const QualType &expectedType, const QualType &actualType) {
    Optional<NullabilityKind> expectedKind = this->nullability(expectedType);
    Optional<NullabilityKind> actualKind = this->nullability(actualType);
    if (expectedKind.getValueOr(NullabilityKind::Unspecified) == NullabilityKind::NonNull) {
      if (actualKind.getValueOr(NullabilityKind::Unspecified) != NullabilityKind::NonNull) {
        expectedType->dump();
        actualType->dump();
        return false;
      }
    }

    return true;
  }

  bool isNonNullExpr(Expr *expr) {
    Expr *e = expr->IgnoreParenCasts();
    if (llvm::dyn_cast<ObjCStringLiteral>(e)) return true;
    if (llvm::dyn_cast<ObjCBoxedExpr>(e)) return true;
    if (llvm::dyn_cast<ObjCArrayLiteral>(e)) return true;
    if (llvm::dyn_cast<ObjCDictionaryLiteral>(e)) return true;

    return false;
  }

  Optional<NullabilityKind> nullability(const QualType &qtype) {
    const Type *type = qtype.getTypePtr();
    if (type) {
      return type->getNullability(Context);
    } else {
      return Optional<NullabilityKind>::create(NULL);
    }
  }

private:
  ASTContext &Context;
  QualType &ReturnType;
};

class NullCheckVisitor : public RecursiveASTVisitor<NullCheckVisitor> {
public:
  NullCheckVisitor(ASTContext &context) : Context(context) {}

  bool VisitDecl(Decl *decl) {
    ObjCMethodDecl *methodDecl = llvm::dyn_cast<ObjCMethodDecl>(decl);
    if (methodDecl) {
      if (methodDecl->hasBody()) {
        QualType returnType = methodDecl->getReturnType();

        MethodBodyChecker checker(Context, returnType);
        checker.TraverseStmt(methodDecl->getBody());
      }
    }
    return true;
  }

private:
  ASTContext &Context;
};

class NullCheckConsumer : public clang::ASTConsumer {
public:
  virtual void HandleTranslationUnit(clang::ASTContext &Context) {
    NullCheckVisitor visitor(Context);
    visitor.TraverseDecl(Context.getTranslationUnitDecl());
  }
};

std::unique_ptr<clang::ASTConsumer> NullCheckAction::CreateASTConsumer(CompilerInstance &Compiler, StringRef InFile) {
  return std::unique_ptr<ASTConsumer>(new NullCheckConsumer);
}

