//===--- ResilienceDiagnostics.cpp - Resilience Inlineability Diagnostics -===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements diagnostics for @inlinable.
//
//===----------------------------------------------------------------------===//

#include "TypeChecker.h"
#include "swift/AST/Attr.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/DeclContext.h"

using namespace swift;
using FragileFunctionKind = TypeChecker::FragileFunctionKind;

std::pair<FragileFunctionKind, bool>
TypeChecker::getFragileFunctionKind(const DeclContext *DC) {
  for (DC = DC->getLocalContext(); DC && DC->isLocalContext();
       DC = DC->getParent()) {
    if (isa<DefaultArgumentInitializer>(DC)) {
      // Default argument generators of public functions cannot reference
      // @usableFromInline declarations; all other fragile function kinds
      // can.
      auto *VD = cast<ValueDecl>(DC->getInnermostDeclarationDeclContext());
      auto access =
        VD->getFormalAccessScope(/*useDC=*/nullptr,
                                 /*treatUsableFromInlineAsPublic=*/false);
      return std::make_pair(FragileFunctionKind::DefaultArgument,
                            !access.isPublic());
    }

    if (isa<PatternBindingInitializer>(DC))
      return std::make_pair(FragileFunctionKind::PropertyInitializer,
                            /*treatUsableFromInlineAsPublic=*/true);

    if (auto *AFD = dyn_cast<AbstractFunctionDecl>(DC)) {
      // If the function is a nested function, we will serialize its body if
      // we serialize the parent's body.
      if (AFD->getDeclContext()->isLocalContext())
        continue;

      // Bodies of public transparent and always-inline functions are
      // serialized, so use conservative access patterns.
      if (AFD->isTransparent())
        return std::make_pair(FragileFunctionKind::Transparent,
                              /*treatUsableFromInlineAsPublic=*/true);

      if (AFD->getAttrs().hasAttribute<InlinableAttr>())
        return std::make_pair(FragileFunctionKind::Inlinable,
                              /*treatUsableFromInlineAsPublic=*/true);

      if (AFD->getAttrs().hasAttribute<AlwaysEmitIntoClientAttr>())
        return std::make_pair(FragileFunctionKind::AlwaysEmitIntoClient,
                              /*treatUsableFromInlineAsPublic=*/true);

      // If a property or subscript is @inlinable, the accessors are
      // @inlinable also.
      if (auto accessor = dyn_cast<AccessorDecl>(AFD)) {
        auto *storage = accessor->getStorage();
        if (storage->getAttrs().getAttribute<InlinableAttr>())
          return std::make_pair(FragileFunctionKind::Inlinable,
                                /*treatUsableFromInlineAsPublic=*/true);
        if (storage->getAttrs().hasAttribute<AlwaysEmitIntoClientAttr>())
          return std::make_pair(FragileFunctionKind::AlwaysEmitIntoClient,
                                /*treatUsableFromInlineAsPublic=*/true);
      }
    }
  }

  llvm_unreachable("Context is not nested inside a fragile function");
}

void TypeChecker::diagnoseInlinableLocalType(const NominalTypeDecl *NTD) {
  auto *DC = NTD->getDeclContext();
  auto expansion = DC->getResilienceExpansion();
  if (expansion == ResilienceExpansion::Minimal) {
    auto kind = getFragileFunctionKind(DC);
    diagnose(NTD, diag::local_type_in_inlinable_function,
             NTD->getFullName(),
             static_cast<unsigned>(kind.first));
  }
}

/// A uniquely-typed boolean to reduce the chances of accidentally inverting
/// a check.
enum class DowngradeToWarning: bool {
  No,
  Yes
};

bool TypeChecker::diagnoseInlinableDeclRef(SourceLoc loc,
                                           const ValueDecl *D,
                                           const DeclContext *DC,
                                           FragileFunctionKind Kind,
                                           bool TreatUsableFromInlineAsPublic) {
  // Do some important fast-path checks that apply to all cases.

  // Local declarations are OK.
  if (D->getDeclContext()->isLocalContext())
    return false;

  // Type parameters are OK.
  if (isa<AbstractTypeParamDecl>(D))
    return false;

  // Check whether the declaration is accessible.
  if (diagnoseInlinableDeclRefAccess(loc, D, DC, Kind,
                                     TreatUsableFromInlineAsPublic))
    return true;

  // Check whether the declaration comes from a publically-imported module.
  if (diagnoseDeclRefExportability(loc, D, DC))
    return true;

  return false;
}

bool TypeChecker::diagnoseInlinableDeclRefAccess(SourceLoc loc,
                                           const ValueDecl *D,
                                           const DeclContext *DC,
                                           FragileFunctionKind Kind,
                                           bool TreatUsableFromInlineAsPublic) {
  // Public declarations are OK.
  if (D->getFormalAccessScope(/*useDC=*/nullptr,
                              TreatUsableFromInlineAsPublic).isPublic())
    return false;

  // Dynamic declarations were mistakenly not checked in Swift 4.2.
  // Do enforce the restriction even in pre-Swift-5 modes if the module we're
  // building is resilient, though.
  if (D->isObjCDynamic() && !Context.isSwiftVersionAtLeast(5) &&
      !DC->getParentModule()->isResilient()) {
    return false;
  }

  // Property initializers that are not exposed to clients are OK.
  if (auto pattern = dyn_cast<PatternBindingInitializer>(DC)) {
    auto bindingIndex = pattern->getBindingIndex();
    auto &patternEntry = pattern->getBinding()->getPatternList()[bindingIndex];
    auto varDecl = patternEntry.getAnchoringVarDecl();
    if (!varDecl->isInitExposedToClients())
      return false;
  }

  DowngradeToWarning downgradeToWarning = DowngradeToWarning::No;

  // Swift 4.2 did not perform any checks for type aliases.
  if (isa<TypeAliasDecl>(D)) {
    if (!Context.isSwiftVersionAtLeast(4, 2))
      return false;
    if (!Context.isSwiftVersionAtLeast(5))
      downgradeToWarning = DowngradeToWarning::Yes;
  }

  auto diagName = D->getFullName();
  bool isAccessor = false;

  // Swift 4.2 did not check accessor accessiblity.
  if (auto accessor = dyn_cast<AccessorDecl>(D)) {
    isAccessor = true;

    if (!Context.isSwiftVersionAtLeast(5))
      downgradeToWarning = DowngradeToWarning::Yes;

    // For accessors, diagnose with the name of the storage instead of the
    // implicit '_'.
    diagName = accessor->getStorage()->getFullName();
  }

  // Swift 5.0 did not check the underlying types of local typealiases.
  // FIXME: Conditionalize this once we have a new language mode.
  if (isa<TypeAliasDecl>(DC))
    downgradeToWarning = DowngradeToWarning::Yes;

  auto diagID = diag::resilience_decl_unavailable;
  if (downgradeToWarning == DowngradeToWarning::Yes)
    diagID = diag::resilience_decl_unavailable_warn;

  diagnose(loc, diagID,
           D->getDescriptiveKind(), diagName,
           D->getFormalAccessScope().accessLevelForDiagnostics(),
           static_cast<unsigned>(Kind),
           isAccessor);

  if (TreatUsableFromInlineAsPublic) {
    diagnose(D, diag::resilience_decl_declared_here,
             D->getDescriptiveKind(), diagName, isAccessor);
  } else {
    diagnose(D, diag::resilience_decl_declared_here_public,
             D->getDescriptiveKind(), diagName, isAccessor);
  }

  return (downgradeToWarning == DowngradeToWarning::No);
}

bool TypeChecker::diagnoseDeclRefExportability(SourceLoc loc,
                                               const ValueDecl *D,
                                               const DeclContext *DC) {
  // We're only interested in diagnosing uses from source files.
  auto userSF = DC->getParentSourceFile();
  if (!userSF)
    return false;

  // If the source file doesn't have any implementation-only imports,
  // we can fast-path this.  In the current language design, we never
  // need to consider the possibility of implementation-only imports
  // from other source files in the module (or indirectly in other modules).
  // TODO: maybe check whether D is from a bridging header?
  if (!userSF->hasImplementationOnlyImports())
    return false;

  auto userModule = userSF->getParentModule();
  auto definingModule = D->getModuleContext();

  // Nothing to diagnose in the very common case of the same module.
  if (userModule == definingModule)
    return false;

  // Nothing to diagnose in the very common case that the module is
  // imported for use in signatures.
  if (!userSF->isImportedImplementationOnly(definingModule))
    return false;

  // TODO: different diagnostics
  diagnose(loc, diag::inlinable_decl_ref_implementation_only,
           D->getDescriptiveKind(), D->getFullName());

  // TODO: notes explaining why
  return true;
}
