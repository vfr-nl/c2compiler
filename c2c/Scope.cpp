/* Copyright 2013 Bas van den Berg
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <assert.h>

#include <clang/Parse/ParseDiagnostic.h>
#include <clang/Sema/SemaDiagnostic.h>

#include "Scope.h"
#include "Package.h"
#include "Decl.h"
#include "Expr.h"
#include "color.h"
#include "StringBuilder.h"

using namespace C2;
using namespace clang;

void ScopeResult::dump() const {
   StringBuilder buffer;
   buffer << "ScopeResult:\n";
   buffer << "  pkg=" << (void*)pkg << '\n';
   buffer << "  decl=" << (void*)decl << '\n';
   buffer << "  ambiguous=" << ambiguous << '\n';
   buffer << "  external=" << external << '\n';
   buffer << "  visible=" << visible << '\n';
   buffer << "  ok=" << ok << '\n';
   fprintf(stderr, "%s", (const char*)buffer);
}


FileScope::FileScope(const std::string& name_, const Pkgs& pkgs_, clang::DiagnosticsEngine& Diags_)
    : pkgName(name_)
    , allPackages(pkgs_)
    , Diags(Diags_)
{
    // add own package to scope
    const Package* myPackage = findAnyPackage(pkgName);
    addPackage(true, pkgName, myPackage);
}

void FileScope::addPackage(bool isLocal, const std::string& name_, const Package* pkg) {
    assert(pkg);
    if (isLocal) {
        locals.push_back(pkg);
    }
    packages[name_] = pkg;
}

const Package* FileScope::findPackage(const std::string& name) const {
    PackagesConstIter iter = packages.find(name);
    if (iter == packages.end()) return 0;
    return iter->second;
}

const Package* FileScope::findAnyPackage(const std::string& name) const {
    PkgsConstIter iter = allPackages.find(name);
    if (iter == allPackages.end()) return 0;
    return iter->second;
}

void FileScope::dump() const {
    fprintf(stderr, "used packages:\n");
    for (PackagesConstIter iter = packages.begin(); iter != packages.end(); ++iter) {
        fprintf(stderr, "  %s (as %s)\n", iter->second->getName().c_str(), iter->first.c_str());
    }
}

int FileScope::checkType(QualType type, bool used_public) {
    assert(type.isValid());

    const Type* T = type.getTypePtr();
    switch (T->getKind()) {
    case Type::BUILTIN:
        return 1;
    case Type::STRUCT:
    case Type::UNION:
        assert(0);
        break;
    case Type::ENUM:
        {
            QualType qt = T->getRefType();
            if (qt.isValid()) return checkType(qt, used_public);
            return 1;
        }
    case Type::USER:
        // TEMP CONST CAST
        return checkUserType((Type*)T, type->getBaseUserType(), used_public);
    case Type::FUNC:
        // TODO
        assert(0 && "TODO");
        break;
    case Type::POINTER:
    case Type::ARRAY:
        return checkType(T->getRefType(), used_public);
    }
}

int FileScope::checkUserType(Type* type, Expr* id, bool used_public) {
    // TODO refactor
    const Package* pkg = 0;
    switch (id->getKind()) {
    case EXPR_IDENTIFIER:   // unqualified
        {
            IdentifierExpr* I = cast<IdentifierExpr>(id);
            ScopeResult res = findSymbol(I->getName());
            if (!res.decl) {
                Diags.Report(I->getLocation(), diag::err_unknown_typename) << I->getName();
                return 1;
            }
            if (res.ambiguous) {
                Diags.Report(I->getLocation(), diag::err_ambiguous_symbol) << I->getName();
                // TODO show alternatives
                return 1;
            }
            if (res.external && !res.decl->isPublic()) {
                Diags.Report(I->getLocation(), diag::err_not_public) << I->getName();
                return 1;
            }
            TypeDecl* td = dyncast<TypeDecl>(res.decl);
            if (!td) {
                Diags.Report(I->getLocation(), diag::err_not_a_typename) << I->getName();
                return 1;
            }
            if (used_public && !res.external && !td->isPublic()) {
                Diags.Report(I->getLocation(), diag::err_non_public_type) << I->getName();
                return 1;
            }
            // ok
            assert(res.pkg && "pkg should be set");
            I->setPackage(res.pkg);
            I->setDecl(res.decl);
            type->setRefType(td->getType());
        }
        break;
    case EXPR_MEMBER:   // fully qualified
        {
            MemberExpr* M = cast<MemberExpr>(id);
            Expr* base = M->getBase();
            IdentifierExpr* pkg_id = cast<IdentifierExpr>(base);
            const std::string& pkgName = pkg_id->getName();
            // check if package exists
            pkg = findPackage(pkgName);
            if (!pkg) {
                // check if used with alias (then fullname is forbidden)
                for (PackagesConstIter iter = packages.begin(); iter != packages.end(); ++iter) {
                    const Package* p = iter->second;
                    if (p->getName() == pkgName) {
                        Diags.Report(pkg_id->getLocation(), diag::err_package_has_alias) << pkgName << iter->first;
                        return 1;
                    }
                }
                // TODO use function
                PkgsConstIter iter = allPackages.find(pkgName);
                if (iter == allPackages.end()) {
                    Diags.Report(pkg_id->getLocation(), diag::err_unknown_package) << pkgName;
                } else {
                    Diags.Report(pkg_id->getLocation(), diag::err_package_not_used) << pkgName;
                }
                return 1;
            }
            // check member
            Expr* member = M->getMember();
            IdentifierExpr* member_id = cast<IdentifierExpr>(member);
            // check Type
            Decl* symbol = pkg->findSymbol(member_id->getName());
            if (!symbol) {
                Diags.Report(member_id->getLocation(), diag::err_unknown_typename) << M->getFullName();
                return 1;
            }
            TypeDecl* td = dyncast<TypeDecl>(symbol);
            if (!td) {
                Diags.Report(member_id->getLocation(), diag::err_not_a_typename) << M->getFullName();
                return 1;
            }
            // if external package, check visibility
            if (isExternal(pkg) && !td->isPublic()) {
                Diags.Report(member_id->getLocation(), diag::err_not_public) << M->getFullName();
                return 1;
            }
            if (used_public && !isExternal(pkg) && !td->isPublic()) {
                Diags.Report(member_id->getLocation(), diag::err_non_public_type) << M->getFullName();
                return 1;
            }
            // ok
            member_id->setPackage(pkg);
            type->setRefType(td->getType());
        }
        break;
    default:
        assert(0);
    }
    return 0;
}

bool FileScope::isExternal(const Package* pkg) const {
    return (pkg->getName() != pkgName);
}

ScopeResult FileScope::findSymbol(const std::string& symbol) const {
    ScopeResult result;
    // symbol can be package name or symbol within package
    const Package* pkg = findPackage(symbol);
    if (pkg) {
        result.pkg = pkg;
        result.external = isExternal(pkg);
        return result;
    }

    // return private symbol only if no public symbol is found
    // ambiguous may also be set with visible = false
    for (LocalsConstIter iter = locals.begin(); iter != locals.end(); ++iter) {
        const Package* pkg = *iter;
        Decl* decl = pkg->findSymbol(symbol);
        if (!decl) continue;

        bool external = isExternal(pkg);
        bool visible = !(external && !decl->isPublic());
        if (result.decl) {  // already found
            if (result.visible == visible) {
                result.ambiguous = true;
                if (result.visible) break;
                continue;
            }
            if (!result.visible) { // replace with visible symbol
                result.decl = decl;
                result.pkg = pkg;
                result.external = external;
                result.ambiguous = false;
                result.visible = visible;
            }
        } else {
            result.decl = decl;
            result.pkg = pkg;
            result.external = external;
            result.visible = visible;
        }
    }
    return result;
}

ScopeResult FileScope::findSymbolInUsed(const std::string& symbol) const {
    ScopeResult result;
    // symbol can be package name or symbol within package
    const Package* pkg = findPackage(symbol);
    if (pkg) {
        result.pkg = pkg;
        result.external = isExternal(pkg);
        return result;
    }

    // search in all used packages
    for (PackagesConstIter iter = packages.begin(); iter != packages.end(); ++iter) {
        const Package* pkg = iter->second;
        Decl* decl = pkg->findSymbol(symbol);
        if (!decl) continue;

        bool external = isExternal(pkg);
        bool visible = !(external && !decl->isPublic());
        // NOTE: dont check ambiguity here (just return first match)
        result.decl = decl;
        result.pkg = pkg;
        result.external = external;
        result.visible = visible;
    }
    return result;
}


Scope::Scope()
    : globals(0)
    , parent(0)
    , Flags(0)
{}

void Scope::InitOnce(FileScope& globals_, Scope* parent_) {
    globals = &globals_;
    parent = parent_;
}

void Scope::Init(unsigned int flags_) {
    Flags = flags_;

    if (parent) {
        if (parent->allowBreak()) Flags |= BreakScope;
        if (parent->allowContinue()) Flags |= ContinueScope;
    }
    decls.clear();
}

ScopeResult Scope::findSymbol(const std::string& symbol) const {
    // search this scope
    ScopeResult result;
    for (DeclsConstIter iter = decls.begin(); iter != decls.end(); ++iter) {
        Decl* D = *iter;
        if (D->getName() == symbol) {
            result.decl = D;
            // TODO fill other result fields
            return result;
        }
    }

    // search parent or globals
    if (parent) return parent->findSymbol(symbol);
    else return globals->findSymbol(symbol);
}

void Scope::addDecl(Decl* d) {
    decls.push_back(d);
}

