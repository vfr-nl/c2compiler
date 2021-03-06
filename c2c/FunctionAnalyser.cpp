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

#include <clang/Parse/ParseDiagnostic.h>
#include <clang/Sema/SemaDiagnostic.h>

#include "FunctionAnalyser.h"
#include "Decl.h"
#include "Expr.h"
#include "Stmt.h"
#include "Package.h"
#include "Scope.h"
#include "color.h"
#include "Utils.h"
#include "StringBuilder.h"

using namespace C2;
using namespace clang;

//#define ANALYSER_DEBUG

#ifdef ANALYSER_DEBUG
#include <iostream>
#define LOG_FUNC std::cerr << ANSI_MAGENTA << __func__ << "()" << ANSI_NORMAL << "\n";
#else
#define LOG_FUNC
#endif

#define MIN(a, b) ((a < b) ? a : b)

// TODO extract to constants.h
const unsigned MAX_TYPENAME = 128;
const unsigned MAX_VARNAME = 64;

// 0 = ok, 1 = loss of precision, 2 sign-conversion, 3=float->integer, 4 incompatible, 5=loss of FP precision
static int type_conversions[14][14] = {
    //U8,  U16, U32, U64, I8, I16, I32, I64, F32, F64, INT, BOOL, STRING, VOID,
    // U8 ->
    {  0,    0,   0,   0,  2,   0,   0,   0,   0,   0,   0,    0,      4,    4},
    // U16 ->
    {  1,    0,   0,   0,  1,   2,   0,   0,   0,   0,   0,    0,      4,    4},
    // U32 ->
    {  1,    1,   0,   0,  1,   1,   2,   0,   0,   0,   2,    0,      4,    4},
    // U64 ->
    {  1,    1,   1,   0,  1,   1,   1,   2,   0,   0,   1,    0,      4,    4},
    //U8,  U16, U32, U64, I8, I16, I32, I64, F32, F64, INT, BOOL, STRING, VOID,
    // I8 ->
    {  2,    2,   2,   2,  0,   0,   0,   0,   0,   0,   0,    0,      4,    4},
    // I16 ->
    {  2,    2,   2,   2,  1,   0,   0,   0,   0,   0,   0,    0,      4,    4},
    // I32 ->
    {  2,    2,   2,   2,  1,   1,   0,   0,   0,   0,   0,    0,      4,    4},
    // I64 ->
    {  2,    2,   2,   2,  1,   1,   1,   0,   0,   0,   1,    0,      4,    4},
    //U8,  U16, U32, U64, I8, I16, I32, I64, F32, F64, INT, BOOL, STRING, VOID,
    // F32 ->
    {  3,    3,   3,   3,  3,   3,   3,   3,   0,   1,   3,    4,      4,    4},
    // F64 ->
    {  3,    3,   3,   3,  3,   3,   3,   3,   5,   0,   3,    4,      4,    4},
    // INT -> (depends on target, for now take I32
    {  2,    2,   2,   2,  1,   1,   0,   0,   0,   0,   0,    0,      4,    4},
    // BOOL ->
    {  0,    0,   0,   0,  2,   0,   0,   0,   0,   0,   0,    0,      4,    4},
    // STRING -> (remove?)
    {  4,    4,   4,   4,  4,   4,   4,   4,   4,   4,   4,    4,      0,    4},
    // VOID ->
    {  4,    4,   4,   4,  4,   4,   4,   4,   4,   4,   4,    4,      4,    0},
};

FunctionAnalyser::FunctionAnalyser(FileScope& scope_,
                                           TypeContext& tc,
                                           clang::DiagnosticsEngine& Diags_)
    : globalScope(scope_)
    , typeContext(tc)
    , scopeIndex(0)
    , curScope(0)
    , Diags(Diags_)
    , errors(0)
    , func(0)
    , inConstExpr(false)
    , constDiagID(0)
{
    Scope* parent = 0;
    for (int i=0; i<MAX_SCOPE_DEPTH; i++) {
        scopes[i].InitOnce(globalScope, parent);
        parent = &scopes[i];
    }
}

FunctionAnalyser::~FunctionAnalyser() {
}

bool FunctionAnalyser::handle(Decl* decl) {
    LOG_FUNC
    switch (decl->getKind()) {
    case DECL_FUNC:
        {
            func = cast<FunctionDecl>(decl);
            EnterScope(Scope::FnScope | Scope::DeclScope);
            // add arguments to new scope
            // Note: duplicate argument names are already checked by Sema
            for (unsigned i=0; i<func->numArgs(); i++) {
                VarDecl* arg = func->getArg(i);
                if (arg->getName() != "") {
                    // check that argument names dont clash with globals
                    ScopeResult res = globalScope.findSymbol(arg->getName());
                    if (res.decl) {
                        // TODO check other attributes?
                        Diags.Report(arg->getLocation(), diag::err_redefinition)
                            << arg->getName();
                        Diags.Report(res.decl->getLocation(), diag::note_previous_definition);
                        continue;
                    }
                    curScope->addDecl(arg);
                }
            }
            analyseCompoundStmt(func->getBody());

            // check for return statement of return value is required
            QualType rtype = func->getReturnType();
            bool need_rvalue = (rtype.getTypePtr() != BuiltinType::get(TYPE_VOID));
            if (need_rvalue) {
                CompoundStmt* compound = func->getBody();
                Stmt* lastStmt = compound->getLastStmt();
                if (!lastStmt || lastStmt->getKind() != STMT_RETURN) {
                    Diags.Report(compound->getRight(), diag::warn_falloff_nonvoid_function);
                }
            }

            ExitScope();
            func = 0;
        }
        break;
    case DECL_VAR:
        {
            // Bit nasty to analyse Initialization values, but we need a lot of shared code
            VarDecl* VD = cast<VarDecl>(decl);
            QualType QT = VD->getType();
            const Type* T = QT.getTypePtr();
            if (T->isArrayType() && T->getArrayExpr()) {
                ConstModeSetter setter(*this, diag::err_vla_decl_in_file_scope);
                EnterScope(0);
                analyseInitExpr(T->getArrayExpr(),  BuiltinType::get(TYPE_INT));
                ExitScope();
            }
            Expr* Init = VD->getInitValue();
            if (Init) {
                ConstModeSetter setter(*this, diag::err_init_element_not_constant);
                EnterScope(0);
                // TEMP CONST CAST
                analyseInitExpr(Init, (Type*)VD->getType().getTypePtr());
                ExitScope();
            }
            if (QT.isConstQualified() && !Init) {
                Diags.Report(VD->getLocation(), diag::err_uninitialized_const_var) << VD->getName();
            }
        }
        break;
    case DECL_ENUMVALUE:
        assert(0 && "TODO");
        break;
    case DECL_TYPE:
    case DECL_STRUCTTYPE:
    case DECL_FUNCTIONTYPE:
    case DECL_ARRAYVALUE:
    case DECL_USE:
        // nothing to do
        break;
    }
    return false;
}

void FunctionAnalyser::EnterScope(unsigned int flags) {
    LOG_FUNC
    assert (scopeIndex < MAX_SCOPE_DEPTH && "out of scopes");
    scopes[scopeIndex].Init(flags);
    curScope = &scopes[scopeIndex];
    scopeIndex++;
}

void FunctionAnalyser::ExitScope() {
    LOG_FUNC
    scopeIndex--;
    Scope* parent = curScope->getParent();
    curScope = parent;
}

void FunctionAnalyser::analyseStmt(Stmt* S, bool haveScope) {
    LOG_FUNC
    switch (S->getKind()) {
    case STMT_RETURN:
        analyseReturnStmt(S);
        break;
    case STMT_EXPR:
        analyseStmtExpr(S);
        break;
    case STMT_IF:
        analyseIfStmt(S);
        break;
    case STMT_WHILE:
        analyseWhileStmt(S);
        break;
    case STMT_DO:
        analyseDoStmt(S);
        break;
    case STMT_FOR:
        analyseForStmt(S);
        break;
    case STMT_SWITCH:
        analyseSwitchStmt(S);
        break;
    case STMT_CASE:
    case STMT_DEFAULT:
        assert(0 && "not done here");
        break;
    case STMT_BREAK:
        analyseBreakStmt(S);
        break;
    case STMT_CONTINUE:
        analyseContinueStmt(S);
        break;
    case STMT_LABEL:
    case STMT_GOTO:
        break;
    case STMT_COMPOUND:
        if (!haveScope) EnterScope(Scope::DeclScope);
        analyseCompoundStmt(S);
        if (!haveScope) ExitScope();
        break;
    }
}

void FunctionAnalyser::analyseCompoundStmt(Stmt* stmt) {
    LOG_FUNC
    CompoundStmt* compound = cast<CompoundStmt>(stmt);
    const StmtList& stmts = compound->getStmts();
    for (unsigned int i=0; i<stmts.size(); i++) {
        analyseStmt(stmts[i]);
    }
}

void FunctionAnalyser::analyseIfStmt(Stmt* stmt) {
    LOG_FUNC
    IfStmt* I = cast<IfStmt>(stmt);
    Expr* cond = I->getCond();
    QualType Q1 = analyseExpr(cond);
    checkConversion(cond->getLocation(), Q1, QualType(BuiltinType::get(TYPE_BOOL)));
    EnterScope(Scope::DeclScope);
    analyseStmt(I->getThen(), true);
    ExitScope();

    Stmt* elseSt = I->getElse();
    if (elseSt) {
        EnterScope(Scope::DeclScope);
        analyseStmt(elseSt, true);
        ExitScope();
    }
}

void FunctionAnalyser::analyseWhileStmt(Stmt* stmt) {
    LOG_FUNC
    WhileStmt* W = cast<WhileStmt>(stmt);
    analyseStmt(W->getCond());
    EnterScope(Scope::BreakScope | Scope::ContinueScope | Scope::DeclScope | Scope::ControlScope);
    analyseStmt(W->getBody(), true);
    ExitScope();

}

void FunctionAnalyser::analyseDoStmt(Stmt* stmt) {
    LOG_FUNC
    DoStmt* D = cast<DoStmt>(stmt);
    EnterScope(Scope::BreakScope | Scope::ContinueScope | Scope::DeclScope);
    analyseStmt(D->getBody());
    ExitScope();
    analyseStmt(D->getCond());
}

void FunctionAnalyser::analyseForStmt(Stmt* stmt) {
    LOG_FUNC
    ForStmt* F = cast<ForStmt>(stmt);
    EnterScope(Scope::BreakScope | Scope::ContinueScope | Scope::DeclScope | Scope::ControlScope);
    if (F->getInit()) analyseStmt(F->getInit());
    if (F->getCond()) analyseExpr(F->getCond());
    if (F->getIncr()) analyseExpr(F->getIncr());
    analyseStmt(F->getBody(), true);
    ExitScope();
}

void FunctionAnalyser::analyseSwitchStmt(Stmt* stmt) {
    LOG_FUNC
    SwitchStmt* S = cast<SwitchStmt>(stmt);
    analyseExpr(S->getCond());
    const StmtList& Cases = S->getCases();
    Stmt* defaultStmt = 0;
    EnterScope(Scope::BreakScope | Scope::SwitchScope);
    for (unsigned i=0; i<Cases.size(); i++) {
        Stmt* C = Cases[i];
        switch (C->getKind()) {
        case STMT_CASE:
            analyseCaseStmt(C);
            break;
        case STMT_DEFAULT:
            if (defaultStmt) {
                Diags.Report(C->getLocation(), diag::err_multiple_default_labels_defined);
                Diags.Report(defaultStmt->getLocation(), diag::note_duplicate_case_prev);
            } else {
                defaultStmt = C;
            }
            analyseDefaultStmt(C);
            break;
        default:
            assert(0);
        }
    }
    ExitScope();
}

void FunctionAnalyser::analyseBreakStmt(Stmt* stmt) {
    LOG_FUNC
    if (!curScope->allowBreak()) {
        BreakStmt* B = cast<BreakStmt>(stmt);
        Diags.Report(B->getLocation(), diag::err_break_not_in_loop_or_switch);
    }
}

void FunctionAnalyser::analyseContinueStmt(Stmt* stmt) {
    LOG_FUNC
    if (!curScope->allowContinue()) {
        ContinueStmt* C = cast<ContinueStmt>(stmt);
        Diags.Report(C->getLocation(), diag::err_continue_not_in_loop);
    }
}

void FunctionAnalyser::analyseCaseStmt(Stmt* stmt) {
    LOG_FUNC
    CaseStmt* C = cast<CaseStmt>(stmt);
    analyseExpr(C->getCond());
    const StmtList& stmts = C->getStmts();
    for (unsigned int i=0; i<stmts.size(); i++) {
        analyseStmt(stmts[i]);
    }
}

void FunctionAnalyser::analyseDefaultStmt(Stmt* stmt) {
    LOG_FUNC
    DefaultStmt* D = cast<DefaultStmt>(stmt);
    const StmtList& stmts = D->getStmts();
    for (unsigned int i=0; i<stmts.size(); i++) {
        analyseStmt(stmts[i]);
    }
}

void FunctionAnalyser::analyseReturnStmt(Stmt* stmt) {
    LOG_FUNC
    ReturnStmt* ret = cast<ReturnStmt>(stmt);
    Expr* value = ret->getExpr();
    QualType rtype = func->getReturnType();
    bool no_rvalue = (rtype.getTypePtr() == BuiltinType::get(TYPE_VOID));
    if (value) {
        QualType type = analyseExpr(value);
        if (no_rvalue) {
            Diags.Report(ret->getLocation(), diag::ext_return_has_expr) << func->getName() << 0;
            // TODO value->getSourceRange()
        } else {
            // TODO check if type and rtype are compatible
        }
    } else {
        if (!no_rvalue) {
            Diags.Report(ret->getLocation(), diag::ext_return_missing_expr) << func->getName() << 0;
        }
    }
}

void FunctionAnalyser::analyseStmtExpr(Stmt* stmt) {
    LOG_FUNC
    Expr* expr = cast<Expr>(stmt);
    analyseExpr(expr);
}

C2::QualType FunctionAnalyser::Decl2Type(Decl* decl) {
    LOG_FUNC
    assert(decl);
    switch (decl->getKind()) {
    case DECL_FUNC:
        {
            FunctionDecl* FD = cast<FunctionDecl>(decl);
            return FD->getType();
        }
    case DECL_VAR:
        {
            VarDecl* VD = cast<VarDecl>(decl);
            return VD->getType();
        }
    case DECL_ENUMVALUE:
        {
            EnumConstantDecl* EC = cast<EnumConstantDecl>(decl);
            return EC->getType();
        }
    case DECL_TYPE:
    case DECL_STRUCTTYPE:
    case DECL_FUNCTIONTYPE:
        {
            TypeDecl* TD = cast<TypeDecl>(decl);
            return TD->getType();
        }
    case DECL_ARRAYVALUE:
    case DECL_USE:
        assert(0);
        break;
    }
    QualType qt;
    return qt;
}

C2::QualType FunctionAnalyser::analyseExpr(Expr* expr) {
    LOG_FUNC
    switch (expr->getKind()) {
    case EXPR_INTEGER_LITERAL:
        // TEMP for now always return type int
        return QualType(BuiltinType::get(TYPE_INT));
    case EXPR_STRING_LITERAL:
        {
            // return type: 'const char*'
            QualType getKind = typeContext.getPointer(BuiltinType::get(TYPE_I8));
            getKind.addConst();
            return getKind;
        }
    case EXPR_BOOL_LITERAL:
        return QualType(BuiltinType::get(TYPE_BOOL));
    case EXPR_CHAR_LITERAL:
        return QualType(BuiltinType::get(TYPE_I8));
    case EXPR_FLOAT_LITERAL:
        // For now always return type float
        return QualType(BuiltinType::get(TYPE_F32));
    case EXPR_CALL:
        return analyseCall(expr);
    case EXPR_IDENTIFIER:
        {
            IdentifierExpr* id = cast<IdentifierExpr>(expr);
            ScopeResult Res = analyseIdentifier(id);
            if (!Res.ok) break;
            if (!Res.decl) break;
            id->setDecl(Res.decl);
            if (Res.pkg) id->setPackage(Res.pkg);
            // NOTE: expr should not be package name (handled above)
            return Decl2Type(Res.decl);
        }
        break;
    case EXPR_INITLIST:
    case EXPR_TYPE:
        // dont handle here
        break;
    case EXPR_DECL:
        analyseDeclExpr(expr);
        break;
    case EXPR_BINOP:
        return analyseBinaryOperator(expr);
    case EXPR_CONDOP:
        return analyseConditionalOperator(expr);
    case EXPR_UNARYOP:
        return analyseUnaryOperator(expr);
    case EXPR_BUILTIN:
        analyseBuiltinExpr(expr);
        break;
    case EXPR_ARRAYSUBSCRIPT:
        return analyseArraySubscript(expr);
    case EXPR_MEMBER:
        return analyseMemberExpr(expr);
    case EXPR_PAREN:
        return analyseParenExpr(expr);
    }
    return QualType();
}

void FunctionAnalyser::analyseInitExpr(Expr* expr, QualType expectedType) {
    LOG_FUNC

    switch (expr->getKind()) {
    case EXPR_INTEGER_LITERAL:
    case EXPR_STRING_LITERAL:
    case EXPR_BOOL_LITERAL:
    case EXPR_CHAR_LITERAL:
    case EXPR_FLOAT_LITERAL:
        // TODO check if compatible
        break;
    case EXPR_CALL:
        // TODO check return type (if void -> size of array has non-integer type 'void')
        assert(constDiagID);
        Diags.Report(expr->getLocation(), constDiagID);
        return;
    case EXPR_IDENTIFIER:
        {
            IdentifierExpr* id = cast<IdentifierExpr>(expr);
            ScopeResult Res = analyseIdentifier(id);
            if (!Res.ok) return;
            if (!Res.decl) return;
            id->setDecl(Res.decl);
            if (Res.pkg) id->setPackage(Res.pkg);
            switch (Res.decl->getKind()) {
            case DECL_FUNC:
                // can be ok for const
                assert(0 && "TODO");
                break;
            case DECL_VAR:
                {
                    VarDecl* VD = cast<VarDecl>(Res.decl);
                    if (inConstExpr) {
                        QualType T = VD->getType();
                        if (!T.isConstQualified()) {
                            Diags.Report(expr->getLocation(), constDiagID);
                            return;
                        }
                    }
                }
                break;
            case DECL_ENUMVALUE:
                // TODO check type compatibility
                break;
            case DECL_TYPE:
            case DECL_STRUCTTYPE:
                assert(0 && "TODO");
                break;
            default:
                assert(0 && "shouldn't come here");
                return;
            }
        }
        break;
    case EXPR_INITLIST:
        analyseInitList(expr, expectedType);
        break;
    case EXPR_TYPE:
        assert(0 && "??");
        break;
    case EXPR_DECL:
        assert(0 && "TODO ERROR");
        break;
    case EXPR_BINOP:
        analyseBinaryOperator(expr);
        break;
    case EXPR_CONDOP:
        analyseConditionalOperator(expr);
        break;
    case EXPR_UNARYOP:
        analyseUnaryOperator(expr);
        break;
    case EXPR_BUILTIN:
        analyseBuiltinExpr(expr);
        break;
    case EXPR_ARRAYSUBSCRIPT:
        analyseArraySubscript(expr);
        break;
    case EXPR_MEMBER:
        // TODO dont allow struct.member, only pkg.constant
        analyseMemberExpr(expr);
        break;
    case EXPR_PAREN:
        analyseParenExpr(expr);
        break;
    }
}

void FunctionAnalyser::analyseInitList(Expr* expr, QualType expectedType) {
    LOG_FUNC
    InitListExpr* I = cast<InitListExpr>(expr);
    assert(expectedType.isValid());

    const Type* type = expectedType.getTypePtr();
    switch (type->getKind()) {
    case Type::USER:
        {
            analyseInitList(expr, type->getRefType());
            return;
        }
    case Type::STRUCT:
    case Type::UNION:
        {
#if 0
            MemberList* members = type->getMembers();
            assert(members);
            // check array member type with each value in initlist
            ExprList& values = I->getValues();
            for (unsigned int i=0; i<values.size(); i++) {
                if (i >= members->size()) {
                    // TODO error: 'excess elements in array initializer'
                    return;
                }
                DeclExpr* member = (*members)[i];
                analyseInitExpr(values[i], member->getType());
            }
#endif
        }
        break;
    case Type::ARRAY:
        {
            // check array member type with each value in initlist
            ExprList& values = I->getValues();
            for (unsigned int i=0; i<values.size(); i++) {
                QualType ref = type->getRefType();
                // TEMP CONST CAST
                analyseInitExpr(values[i], (Type*)ref.getTypePtr());
            }
        }
        break;
    default:
        {
        char typeName[MAX_TYPENAME];
        StringBuilder buf(MAX_TYPENAME, typeName);
        type->DiagName(buf);
        Diags.Report(expr->getLocation(), diag::err_invalid_type_initializer_list) << typeName;
        }
        break;
    }
}

void FunctionAnalyser::analyseDeclExpr(Expr* expr) {
    LOG_FUNC
    DeclExpr* DE = cast<DeclExpr>(expr);
    VarDecl* decl = DE->getDecl();

    // check type and convert User types
    QualType type = decl->getType();
    // TODO CONST CAST
    errors += globalScope.checkType((Type*)type.getTypePtr(), false);

    // check name
    ScopeResult res = curScope->findSymbol(decl->getName());
    if (res.decl) {
        // TODO check other attributes?
        Diags.Report(decl->getLocation(), diag::err_redefinition)
            << decl->getName();
        Diags.Report(res.decl->getLocation(), diag::note_previous_definition);
        return;
    }
    // check initial value
    Expr* initialValue = decl->getInitValue();
    if (initialValue) {
        // TODO check initial value type
        analyseExpr(initialValue);
    }
    if (type.isConstQualified() && !initialValue) {
        Diags.Report(decl->getLocation(), diag::err_uninitialized_const_var) << decl->getName();
    }
    curScope->addDecl(decl);
}

QualType FunctionAnalyser::analyseBinaryOperator(Expr* expr) {
    LOG_FUNC
    BinaryOperator* binop = cast<BinaryOperator>(expr);
    QualType TLeft = analyseExpr(binop->getLHS());
    QualType TRight = analyseExpr(binop->getRHS());
    // assigning to 'A' from incompatible type 'B'
    // diag::err_typecheck_convert_incompatible
    if (TLeft.isNull() || TRight.isNull()) return QualType();

    switch (binop->getOpcode()) {
    case BO_PtrMemD:
    case BO_PtrMemI:
        assert(0 && "unhandled binary operator type");
        break;
    case BO_Mul:
    case BO_Div:
    case BO_Rem:
    case BO_Add:
    case BO_Sub:
        // TODO return largetst witdth of left/right (long*short -> long)
        // TEMP just return INT
        return QualType(BuiltinType::get(TYPE_INT));
    case BO_Shl:
    case BO_Shr:
        return TLeft;
    case BO_LE:
    case BO_LT:
    case BO_GE:
    case BO_GT:
    case BO_NE:
    case BO_EQ:
        return QualType(BuiltinType::get(TYPE_BOOL));
    case BO_And:
    case BO_Xor:
    case BO_Or:
        return TLeft;
    case BO_LAnd:
    case BO_LOr:
        return QualType(BuiltinType::get(TYPE_BOOL));
    case BO_Assign:
        checkConversion(binop->getLocation(), TRight, TLeft);
        checkAssignment(binop->getLHS(), TLeft);
        return TLeft;
    case BO_MulAssign:
    case BO_DivAssign:
    case BO_RemAssign:
    case BO_AddAssign:
    case BO_SubAssign:
    case BO_ShlAssign:
    case BO_ShrAssign:
    case BO_AndAssign:
    case BO_XorAssign:
    case BO_OrAssign:
        checkAssignment(binop->getLHS(), TLeft);
        return TLeft;
    case BO_Comma:
        assert(0 && "unhandled binary operator type");
        break;
    }
    return QualType();
}

QualType FunctionAnalyser::analyseConditionalOperator(Expr* expr) {
    LOG_FUNC
    ConditionalOperator* condop = cast<ConditionalOperator>(expr);
    analyseExpr(condop->getCond());
    QualType TLeft = analyseExpr(condop->getLHS());
    analyseExpr(condop->getRHS());
    // TODO also check type of RHS
    return TLeft;
}

QualType FunctionAnalyser::analyseUnaryOperator(Expr* expr) {
    LOG_FUNC
    UnaryOperator* unaryop = cast<UnaryOperator>(expr);
    QualType LType = analyseExpr(unaryop->getExpr());
    if (LType.isNull()) return 0;
    switch (unaryop->getOpcode()) {
    case UO_AddrOf:
        return typeContext.getPointer(LType);
    case UO_Deref:
        // TODO handle user types
        if (!LType.isPointerType()) {
            char typeName[MAX_TYPENAME];
            StringBuilder buf(MAX_TYPENAME, typeName);
            LType.DiagName(buf);
            Diags.Report(unaryop->getOpLoc(), diag::err_typecheck_indirection_requires_pointer)
                << buf;
            return 0;
        }
        break;
    default:
        break;
    }
    return LType;
}

void FunctionAnalyser::analyseBuiltinExpr(Expr* expr) {
    LOG_FUNC
    BuiltinExpr* func = cast<BuiltinExpr>(expr);
    analyseExpr(func->getExpr());
    if (func->isSizeof()) { // sizeof()
        // TODO can also be type (for sizeof)
    } else { // elemsof()
        Expr* E = func->getExpr();
        IdentifierExpr* I = cast<IdentifierExpr>(E);
        Decl* D = I->getDecl();
        // should be VarDecl(for array/enum) or TypeDecl(array/enum)
        switch (D->getKind()) {
        case DECL_FUNC:
             fprintf(stderr, "TODO Function err\n");
            // ERROR
            return;
        case DECL_VAR:
            {
                VarDecl* VD = cast<VarDecl>(D);
                QualType Q = VD->getType();
                if (!Q.isArrayType() && !Q.isEnumType()) {
                    StringBuilder msg;
                    Q.DiagName(msg);
                    Diags.Report(I->getLocation(), diag::err_invalid_elemsof_type)
                        << msg;
                }
                return;
            }
        case DECL_ENUMVALUE:
            // ERROR
            break;
        case DECL_TYPE:
        case DECL_STRUCTTYPE:
        case DECL_FUNCTIONTYPE:
            assert(0 && "TODO");
            break;
        case DECL_ARRAYVALUE:
        case DECL_USE:
            assert(0);
            break;
        }
    }
}

QualType FunctionAnalyser::analyseArraySubscript(Expr* expr) {
    LOG_FUNC
    ArraySubscriptExpr* sub = cast<ArraySubscriptExpr>(expr);
    QualType LType = analyseExpr(sub->getBase());
    if (LType.isNull()) return 0;
    // TODO this should be done in analyseExpr()
    QualType LType2 = resolveUserType(LType);
    if (LType2.isNull()) return 0;
    if (!LType2.isSubscriptable()) {
        Diags.Report(expr->getLocation(), diag::err_typecheck_subscript);
        return 0;
    }
    analyseExpr(sub->getIndex());
    return LType2.getTypePtr()->getRefType();
}

QualType FunctionAnalyser::analyseMemberExpr(Expr* expr) {
    LOG_FUNC
    MemberExpr* M = cast<MemberExpr>(expr);
    IdentifierExpr* member = M->getMember();

    bool isArrow = M->isArrow();
    // we dont know what we're looking at here, can be:
    // pkg.type
    // pkg.var
    // pkg.func
    // var(Type=struct>.member
    // var[index].member
    // var->member
    // At least check if it exists for now
    Expr* base = M->getBase();
    if (base->getKind() == EXPR_IDENTIFIER) {
        IdentifierExpr* base_id = cast<IdentifierExpr>(base);
        ScopeResult SR = analyseIdentifier(base_id);
        if (!SR.ok) return QualType();
        if (SR.decl) {
            switch (SR.decl->getKind()) {
            case DECL_FUNC:
            case DECL_TYPE:
            case DECL_FUNCTIONTYPE:
                fprintf(stderr, "error: member reference base 'type' is not a structure, union or package\n");
                return QualType();
            case DECL_STRUCTTYPE:
                assert(0);  // will always be UnresolvedType
                break;
            case DECL_VAR:
                {
                    // TODO extract to function?
                    VarDecl* VD = cast<VarDecl>(SR.decl);
                    QualType T = VD->getType();
                    assert(T.isValid());  // analyser should set

                    if (isArrow) {
                        if (!T.isPointerType()) {
                            fprintf(stderr, "TODO using -> with non-pointer type\n");
                            // continue analysing
                        } else {
                            // deref
                            T = T->getRefType();
                        }
                    } else {
                        if (T.isPointerType()) {
                            fprintf(stderr, "TODO using . with pointer type\n");
                            // just deref and continue for now
                            T = T.getTypePtr()->getRefType();
                            // TODO qualifiers?
                        }
                    }
                    if (T.isUserType()) {
                        T = T.getTypePtr()->getRefType();
                        // TODO qualifiers?
                        assert(T.isValid() && "analyser should set refType");
                    }
                    // check if struct/union type
                    // TODO do the lookup once during declaration. Just have pointer to real Type here.
                    // This cannot be a User type (but can be struct/union etc)
                    if (!T->isStructOrUnionType()) {
                        // TODO need loc of Op, for now take member
/*
                        Diags.Report(member->getLocation(), diag::err_typecheck_member_reference_struct_union)
                            << T->toString() << M->getSourceRange() << member->getLocation();
*/
                        fprintf(stderr, "error: type of symbol '%s' is not a struct or union\n",
                            base_id->getName().c_str());
                        return QualType();
                    }
                    return analyseMember(T, member);
                }
                break;
            case DECL_ENUMVALUE:
                assert(0 && "TODO");
                break;
            case DECL_ARRAYVALUE:
            case DECL_USE:
                assert(0);
                break;
            }
        } else if (SR.pkg) {
            if (isArrow) {
                fprintf(stderr, "TODO ERROR: cannot use -> for package access\n");
                // continue checking
            }
            // lookup member in package
            Decl* D = SR.pkg->findSymbol(member->getName());
            if (!D) {
                Diags.Report(member->getLocation(), diag::err_unknown_package_symbol)
                    << SR.pkg->getName() << member->getName();
                return QualType();
            }
            if (SR.external && !D->isPublic()) {
                Diags.Report(member->getLocation(), diag::err_not_public)
                    << Utils::fullName(SR.pkg->getName(), D->getName());
                return QualType();
            }
            member->setPackage(SR.pkg);
            return Decl2Type(D);
        }
    } else {
        QualType LType = analyseExpr(base);
        if (LType.isNull()) return QualType();
        // TODO this should be done in analyseExpr()
        QualType LType2 = resolveUserType(LType);
        if (LType2.isNull()) return QualType();
        if (!LType2.isStructOrUnionType()) {
            fprintf(stderr, "error: not a struct or union type\n");
            LType2->dump();
            return QualType();
        }
        return analyseMember(LType2, member);
    }
    return QualType();
}

QualType FunctionAnalyser::analyseMember(QualType T, IdentifierExpr* member) {
    LOG_FUNC
    StructTypeDecl* S = T->getStructDecl();
    Decl* match = S->find(member->getName());
    if (match) {
        // NOT very nice, structs can have VarDecls or StructTypeDecls
        if (isa<VarDecl>(match)) {
            return cast<VarDecl>(match)->getType();
        }
        if (isa<StructTypeDecl>(match)) {
            return cast<StructTypeDecl>(match)->getType();
        }
        assert(0);
        return QualType();
    }
    char temp1[MAX_TYPENAME];
    StringBuilder buf1(MAX_TYPENAME, temp1);
    T->DiagName(buf1);

    char temp2[MAX_VARNAME];
    StringBuilder buf2(MAX_VARNAME, temp2);
    buf2 << '\'' << member->getName() << '\'';
    Diags.Report(member->getLocation(), diag::err_no_member) << temp2 << temp1;
    return QualType();
}

QualType FunctionAnalyser::analyseParenExpr(Expr* expr) {
    LOG_FUNC
    ParenExpr* P = cast<ParenExpr>(expr);
    return analyseExpr(P->getExpr());
}

QualType FunctionAnalyser::analyseCall(Expr* expr) {
    LOG_FUNC
    CallExpr* call = cast<CallExpr>(expr);
    // analyse function
    QualType LType = analyseExpr(call->getFn());
    if (LType.isNull()) {
        fprintf(stderr, "CALL unknown function (already error)\n");
        call->getFn()->dump();
        return QualType();
    }
    // TODO this should be done in analyseExpr()
    QualType LType2 = resolveUserType(LType);
    if (LType2.isNull()) return QualType();
    if (!LType2.isFuncType()) {
        fprintf(stderr, "error: NOT a function type TODO\n");
        char typeName[MAX_TYPENAME];
        StringBuilder buf(MAX_TYPENAME, typeName);
        LType2.DiagName(buf);
        Diags.Report(call->getLocation(), diag::err_typecheck_call_not_function) << typeName;
        return QualType();
    }

    FunctionDecl* func = LType2->getDecl();
    unsigned protoArgs = func->numArgs();
    unsigned callArgs = call->numArgs();
    unsigned minArgs = MIN(protoArgs, callArgs);
    for (unsigned i=0; i<minArgs; i++) {
        Expr* argGiven = call->getArg(i);
        QualType typeGiven = analyseExpr(argGiven);
        VarDecl* argFunc = func->getArg(i);
        QualType argType = argFunc->getType();
        // TODO match types
    }
    if (callArgs > protoArgs) {
        // more args given, check if function is variadic
        if (!func->isVariadic()) {
            Expr* arg = call->getArg(minArgs);
            unsigned msg = diag::err_typecheck_call_too_many_args;
            if (func->hasDefaultArgs()) msg = diag::err_typecheck_call_too_many_args_at_most;
            Diags.Report(arg->getLocation(), msg)
                << 0 << protoArgs << callArgs;
            return QualType();
        }
        for (unsigned i=minArgs; i<callArgs; i++) {
            Expr* argGiven = call->getArg(i);
            QualType typeGiven = analyseExpr(argGiven);
            if (typeGiven->isVoid()) {
                fprintf(stderr, "ERROR: (TODO) passing 'void' to parameter of incompatible type '...'\n");
                //Diags.Report(argGiven->getLocation(), diag::err_typecheck_convert_incompatible)
                //    << "from" << "to" << 1;// << argGiven->getLocation();
#warning "TODO check if not void"
            }
        }
    } else if (callArgs < protoArgs) {
        // less args given, check for default argument values
        for (unsigned i=minArgs; i<protoArgs; i++) {
            VarDecl* arg = func->getArg(i);
            if (!arg->getInitValue()) {
                if (func->hasDefaultArgs()) {
                    protoArgs = func->minArgs();
                    Diags.Report(arg->getLocation(), diag::err_typecheck_call_too_few_args_at_least)
                        << 0 << protoArgs << callArgs;
                } else {
                    Diags.Report(arg->getLocation(), diag::err_typecheck_call_too_few_args)
                        << 0 << protoArgs << callArgs;
                }
                return QualType();
            }
        }
    }
    return func->getReturnType();
}

ScopeResult FunctionAnalyser::analyseIdentifier(IdentifierExpr* id) {
    LOG_FUNC
    ScopeResult res = curScope->findSymbol(id->getName());
    if (res.decl) {
        if (res.ambiguous) {
            res.ok = false;
            fprintf(stderr, "TODO ambiguous variable\n");
            // TODO show alternatives
            return res;
        }
        if (!res.visible) {
            res.ok = false;
            Diags.Report(id->getLocation(), diag::err_not_public) << id->getName();
            return res;
        }
    } else {
        if (res.pkg) {
            // symbol is package
        } else {
            res.ok = false;
            Diags.Report(id->getLocation(), diag::err_undeclared_var_use)
                << id->getName();
            ScopeResult res2 = globalScope.findSymbolInUsed(id->getName());
            if (res2.decl) {
                Diags.Report(res2.decl->getLocation(), diag::note_function_suggestion)
                    << Utils::fullName(res2.pkg->getName(), id->getName());
            }
        }
    }
    return res;
}

void FunctionAnalyser::checkConversion(SourceLocation Loc, QualType from, QualType to) {
#ifdef ANALYSER_DEBUG
    StringBuilder buf;
    buf <<ANSI_MAGENTA << __func__ << "() converversion ";
    from.DiagName(buf);
    buf << " to ";
    to.DiagName(buf);
    buf << ANSI_NORMAL;
    fprintf(stderr, "%s\n", (const char*)buf);
#endif
    // TEMP only check float -> bool
    const Type* tfrom = from.getTypePtr();
    const Type* tto = to.getTypePtr();
    // TODO use getCanonicalType()
    if (tfrom->isBuiltinType() && tto->isBuiltinType()) {
        C2Type bi_from = tfrom->getBuiltinType();
        C2Type bi_to = tto->getBuiltinType();
        int rule = type_conversions[bi_from][bi_to];
        // 0 = ok, 1 = loss of precision, 2 sign-conversion, 3=float->integer, 4 incompatible, 5 loss of FP prec.
        // TODO use matrix with allowed conversions: 3 options: ok, error, warn
        if (rule == 0) return;

        int errorMsg = 0;
        switch (rule) {
        case 1: // loss of precision
            errorMsg = diag::warn_impcast_integer_precision;
            break;
        case 2: // sign-conversion
            errorMsg = diag::warn_impcast_integer_sign;
        case 3: // float->integer
            errorMsg = diag::warn_impcast_float_integer;
            break;
        case 4: // incompatible
            errorMsg = diag::err_illegal_type_conversion;
            break;
        case 5: // loss of fp-precision
            errorMsg = diag::warn_impcast_float_precision;
            break;
        default:
            assert(0 && "should not come here");
        }
        StringBuilder buf1(MAX_TYPENAME);
        StringBuilder buf2(MAX_TYPENAME);
        from.DiagName(buf1);
        to.DiagName(buf2);
        Diags.Report(Loc, errorMsg) << buf1 << buf2;
    }
}

void FunctionAnalyser::checkAssignment(Expr* assignee, QualType TLeft) {
    if (TLeft.isConstQualified()) {
        Diags.Report(assignee->getLocation(), diag::err_typecheck_assign_const);
    }
}

C2::QualType FunctionAnalyser::resolveUserType(QualType T) {
    if (T->isUserType()) {
        QualType t2 = T.getTypePtr()->getRefType();
        assert(t2.isValid());
        // TODO Qualifiers correct?
        return t2;
    }
    return T;
}

void FunctionAnalyser::pushMode(unsigned DiagID) {
    LOG_FUNC
    assert(inConstExpr == false);
    inConstExpr = true;
    constDiagID = DiagID;
}

void FunctionAnalyser::popMode() {
    LOG_FUNC
    assert(inConstExpr == true);
    inConstExpr = false;
    constDiagID = 0;
}

