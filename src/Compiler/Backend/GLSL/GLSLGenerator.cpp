/*
 * GLSLGenerator.cpp
 * 
 * This file is part of the XShaderCompiler project (Copyright (c) 2014-2016 by Lukas Hermanns)
 * See "LICENSE.txt" for license information.
 */

#include "GLSLGenerator.h"
#include "GLSLExtensionAgent.h"
#include "GLSLConverter.h"
#include "GLSLKeywords.h"
#include "GLSLIntrinsics.h"
#include "GLSLHelper.h"
#include "ReferenceAnalyzer.h"
#include "ControlPathAnalyzer.h"
#include "TypeDenoter.h"
#include "AST.h"
#include "Exception.h"
#include "Helper.h"
#include <initializer_list>
#include <algorithm>
#include <cctype>


namespace Xsc
{


GLSLGenerator::GLSLGenerator(Log* log) :
    Generator{ log }
{
}

void GLSLGenerator::GenerateCodePrimary(
    Program& program, const ShaderInput& inputDesc, const ShaderOutput& outputDesc)
{
    /* Store parameters */
    shaderTarget_       = inputDesc.shaderTarget;
    versionOut_         = outputDesc.shaderVersion;
    allowExtensions_    = outputDesc.options.allowExtensions;
    allowLineMarks_     = outputDesc.formatting.lineMarks;
    nameManglingPrefix_ = outputDesc.formatting.prefix;
    stats_              = outputDesc.statistics;

    if (program.entryPointRef)
    {
        try
        {
            /* Mark all control paths */
            {
                ControlPathAnalyzer pathAnalyzer;
                pathAnalyzer.MarkControlPaths(program);
            }

            /* Convert AST for GLSL code generation */
            {
                GLSLConverter converter;
                converter.Convert(program, inputDesc.shaderTarget, outputDesc.formatting.prefix);
            }

            /* Mark all reachable AST nodes */
            {
                ReferenceAnalyzer refAnalyzer;
                refAnalyzer.MarkReferencesFromEntryPoint(program);
            }

            /* Write header */
            if (inputDesc.entryPoint.empty())
                Comment("GLSL " + TargetToString(shaderTarget_));
            else
                Comment("GLSL " + TargetToString(shaderTarget_) + " \"" + inputDesc.entryPoint + "\"");
        
            Comment("Generated by XShaderCompiler");

            Comment(TimePoint());
            Blank();

            /* Visit program AST */
            Visit(&program);
        }
        catch (const Report& e)
        {
            throw e;
        }
        catch (const ASTRuntimeError& e)
        {
            Error(e.what(), e.GetAST());
        }
        catch (const std::exception& e)
        {
            Error(e.what());
        }
    }
    else
        Error("entry point \"" + inputDesc.entryPoint + "\" not found");
}


/*
 * ======= Private: =======
 */

void GLSLGenerator::Comment(const std::string& text)
{
    WriteLn("// " + text);
}

void GLSLGenerator::Version(int versionNumber)
{
    WriteLn("#version " + std::to_string(versionNumber));
}

void GLSLGenerator::Line(int lineNumber)
{
    if (allowLineMarks_)
        WriteLn("#line " + std::to_string(lineNumber));
}

void GLSLGenerator::Line(const TokenPtr& tkn)
{
    Line(tkn->Pos().Row());
}

void GLSLGenerator::Line(const AST* ast)
{
    Line(ast->area.Pos().Row());
}

void GLSLGenerator::WriteExtension(const std::string& extensionName)
{
    WriteLn("#extension " + extensionName + " : enable");// "require" or "enable"
}

void GLSLGenerator::WriteVersionAndExtensions(Program& ast)
{
    try
    {
        /* Determine all required GLSL extensions with the GLSL extension agent */
        GLSLExtensionAgent extensionAgent;
        auto requiredExtensions = extensionAgent.DetermineRequiredExtensions(
            ast, versionOut_, shaderTarget_, allowExtensions_
        );

        /* Write GLSL version */
        Version(static_cast<int>(versionOut_));
        Blank();

        /* Write all required extensions */
        if (!requiredExtensions.empty())
        {
            for (const auto& ext : requiredExtensions)
                WriteExtension(ext);
            Blank();
        }
    }
    catch (const std::exception& e)
    {
        Error(e.what());
    }
}

void GLSLGenerator::WriteReferencedIntrinsics(Program& ast)
{
    auto Used = [&ast](Intrinsic intr)
    {
        return (ast.usedIntrinsics.find(intr) != ast.usedIntrinsics.end());
    };

    if (Used(Intrinsic::Clip))
        WriteClipIntrinsics();
}

void GLSLGenerator::WriteClipIntrinsics()
{
    WriteLn("void clip(float x) { if (x < 0.0) discard; }");

    for (const auto& typeName : std::vector<std::string>{ "vec2", "vec3", "vec4" })
        WriteLn("void clip(" + typeName + " x) { if (any(lessThan(x, " + typeName + "(0.0)))) discard; }");

    Blank();
}

void GLSLGenerator::OpenScope()
{
    WriteLn("{");
    IncIndent();
}

void GLSLGenerator::CloseScope(bool semicolon)
{
    DecIndent();
    WriteLn(semicolon ? "};" : "}");
}

void GLSLGenerator::ValidateRegisterPrefix(const std::string& registerName, char prefix, const AST* ast)
{
    if (registerName.empty() || registerName[0] != prefix)
    {
        Error(
            "invalid register prefix '" + std::string(1, registerName[0]) +
            "' (expected '" + std::string(1, prefix) + "')",
            ast
        );
    }
}

int GLSLGenerator::RegisterIndex(const std::string& registerName)
{
    return std::stoi(registerName.substr(1));
}

//TODO: move this to GLSLConverter
#if 1
std::string GLSLGenerator::BRegister(const std::string& registerName, const AST* ast)
{
    ValidateRegisterPrefix(registerName, 'b', ast);
    return registerName.substr(1);
}

std::string GLSLGenerator::TRegister(const std::string& registerName, const AST* ast)
{
    ValidateRegisterPrefix(registerName, 't', ast);
    return registerName.substr(1);
}

std::string GLSLGenerator::SRegister(const std::string& registerName, const AST* ast)
{
    ValidateRegisterPrefix(registerName, 's', ast);
    return registerName.substr(1);
}

std::string GLSLGenerator::URegister(const std::string& registerName, const AST* ast)
{
    ValidateRegisterPrefix(registerName, 'u', ast);
    return registerName.substr(1);
}
#endif

bool GLSLGenerator::MustResolveStruct(StructDecl* ast) const
{
    return MustResolveStructForTarget(shaderTarget_, ast);
}

bool GLSLGenerator::IsVersionOut(int version) const
{
    return static_cast<int>(versionOut_) >= version;
}

/* ------- Visit functions ------- */

#define IMPLEMENT_VISIT_PROC(AST_NAME) \
    void GLSLGenerator::Visit##AST_NAME(AST_NAME* ast, void* args)

IMPLEMENT_VISIT_PROC(Program)
{
    /* Write version and required extensions first */
    WriteVersionAndExtensions(*ast);

    /* Write 'gl_FragCoord' layout */
    if (shaderTarget_ == ShaderTarget::FragmentShader)
    {
        BeginLn();
        {
            Write("layout(origin_upper_left");
            if (GetProgram()->flags(Program::hasSM3ScreenSpace))
                Write(", pixel_center_integer");
            Write(") in vec4 gl_FragCoord;");
        }
        EndLn();
        Blank();
    }

    /* Write entry point attributes */
    if (!ast->entryPointRef->attribs.empty())
    {
        for (auto& attrib : ast->entryPointRef->attribs)
            WriteAttribute(attrib.get());
        Blank();
    }

    /* Append default helper macros and functions */
    WriteReferencedIntrinsics(*ast);

    if (shaderTarget_ == ShaderTarget::VertexShader)
        WriteGlobalInputSemantics();
    else if (shaderTarget_ == ShaderTarget::FragmentShader)
        WriteGlobalOutputSemantics();

    Visit(ast->globalStmnts);
}

IMPLEMENT_VISIT_PROC(CodeBlock)
{
    OpenScope();
    {
        Visit(ast->stmnts);
    }
    CloseScope();
}

IMPLEMENT_VISIT_PROC(FunctionCall)
{
    if (ast->intrinsic == Intrinsic::Mul)
        WriteFunctionCallIntrinsicMul(ast);
    else if (ast->intrinsic == Intrinsic::Rcp)
        WriteFunctionCallIntrinsicRcp(ast);
    else if (ast->intrinsic >= Intrinsic::InterlockedAdd && ast->intrinsic <= Intrinsic::InterlockedXor)
        WriteFunctionCallIntrinsicAtomic(ast);
    else
        WriteFunctionCallStandard(ast);
}

IMPLEMENT_VISIT_PROC(SwitchCase)
{
    /* Write case header */
    if (ast->expr)
    {
        BeginLn();
        {
            Write("case ");
            Visit(ast->expr);
            Write(":");
        }
        EndLn();
    }
    else
        WriteLn("default:");

    /* Write statement list */
    IncIndent();
    {
        Visit(ast->stmnts);
    }
    DecIndent();
}

/* --- Variables --- */

IMPLEMENT_VISIT_PROC(VarType)
{
    if (ast->structDecl)
        Visit(ast->structDecl);
    else
        WriteTypeDenoter(*ast->typeDenoter, ast);
}

IMPLEMENT_VISIT_PROC(VarIdent)
{
    WriteVarIdent(ast);
}

/* --- Declarations --- */

IMPLEMENT_VISIT_PROC(VarDecl)
{
    Write(ast->ident);
    WriteArrayDims(ast->arrayDims);

    if (ast->initializer)
    {
        Write(" = ");
        Visit(ast->initializer);
    }
}

IMPLEMENT_VISIT_PROC(StructDecl)
{
    if (!MustResolveStruct(ast))
    {
        bool semicolon = (args != nullptr ? *reinterpret_cast<bool*>(&args) : false);

        /* Write all nested structures (if this is the root structure) */
        if (!ast->flags(StructDecl::isNestedStruct))
        {
            /* Write nested structres in child-to-parent order */
            for (auto nestedStruct = ast->nestedStructDeclRefs.rbegin(); nestedStruct != ast->nestedStructDeclRefs.rend(); ++nestedStruct)
            {
                WriteStructDecl(*nestedStruct, true, true);
                Blank();
            }
        }

        /* Write declaration of this structure (without nested structures) */
        WriteStructDecl(ast, semicolon);
    }
}

/* --- Declaration statements --- */

IMPLEMENT_VISIT_PROC(FunctionDecl)
{
    /* Is this function reachable from the entry point? */
    if (!ast->flags(AST::isReachable))
    {
        /* Check for valid control paths */
        if (ast->flags(FunctionDecl::hasNonReturnControlPath))
            Warning("not all control paths in unreferenced function '" + ast->ident + "' return a value", ast);
        return;
    }

    /* Check for valid control paths */
    if (ast->flags(FunctionDecl::hasNonReturnControlPath))
        Error("not all control paths in function '" + ast->ident + "' return a value", ast);

    /* Write line */
    Line(ast);

    /* Write function header */
    BeginLn();
    {
        if (ast->flags(FunctionDecl::isEntryPoint))
            Write("void main()");
        else
        {
            Visit(ast->returnType);
            Write(" " + ast->ident + "(");

            /* Write parameters */
            for (size_t i = 0; i < ast->parameters.size(); ++i)
            {
                WriteParameter(ast->parameters[i].get());
                if (i + 1 < ast->parameters.size())
                    Write(", ");
            }

            Write(")");

            if (!ast->codeBlock)
            {
                /*
                This is only a function forward declaration
                -> finish with line terminator
                */
                Write(";");
            }
        }
    }
    EndLn();

    if (ast->codeBlock)
    {
        /* Write function body */
        if (ast->flags(FunctionDecl::isEntryPoint))
        {
            OpenScope();
            {
                /* Write input/output parameters of system values as local variables */
                WriteLocalInputSemantics();
                WriteLocalOutputSemantics();

                /* Write code block (without additional scope) */
                isInsideEntryPoint_ = true;
                {
                    Visit(ast->codeBlock->stmnts);
                }
                isInsideEntryPoint_ = false;

                /* Is the last statement a return statement? */
                if (ast->codeBlock->stmnts.empty() || ast->codeBlock->stmnts.back()->Type() != AST::Types::ReturnStmnt)
                {
                    /* Write output semantic at the end of the code block, if no return statement was written before */
                    WriteOutputSemanticsAssignment(nullptr);
                }
            }
            CloseScope();
        }
        else
        {
            /* Write default code block */
            Visit(ast->codeBlock);
        }
    }

    Blank();
}

IMPLEMENT_VISIT_PROC(BufferDeclStmnt)
{
    if (!ast->flags(AST::isReachable))
        return;

    /* Write uniform buffer header */
    Line(ast);

    BeginLn();
    {
        Write("layout(std140");

        if (auto slotRegister = Register::GetForTarget(ast->slotRegisters, shaderTarget_))
            Write(", binding = " + std::to_string(slotRegister->slot));

        Write(") uniform ");
        Write(ast->ident);
    }
    EndLn();

    OpenScope();
    {
        Visit(ast->members);
    }
    CloseScope(true);

    Blank();
}

IMPLEMENT_VISIT_PROC(TextureDeclStmnt)
{
    if (!ast->flags(AST::isReachable))
        return;

    /* Determine GLSL sampler type */
    auto samplerType = BufferTypeToGLSLKeyword(ast->textureType);
    if (!samplerType)
        Error("failed to map texture type to GLSL sampler type", ast);

    /* Write texture samplers */
    for (auto& texDecl : ast->textureDecls)
    {
        if (texDecl->flags(AST::isReachable))
        {
            BeginLn();
            {
                int binding = -1;

                /* Write uniform declaration */
                if (auto slotRegister = Register::GetForTarget(texDecl->slotRegisters, shaderTarget_))
                    Write("layout(binding = " + std::to_string(slotRegister->slot) + ") ");

                Write("uniform " + *samplerType + " " + texDecl->ident + ";");

                /* Write output statistics */
                if (stats_)
                    stats_->textures.push_back({ texDecl->ident, binding });
            }
            EndLn();
        }
    }

    Blank();
}

IMPLEMENT_VISIT_PROC(StructDeclStmnt)
{
    if (!ast->structDecl->flags(AST::isReachable))
        return;

    if (!MustResolveStruct(ast->structDecl.get()))
    {
        Line(ast);

        bool semicolon = true;
        Visit(ast->structDecl, &semicolon);

        Blank();
    }
}

IMPLEMENT_VISIT_PROC(VarDeclStmnt)
{
    auto varDecls = ast->varDecls;

    //TODO: refactor this!
    #if 1
    for (auto it = varDecls.begin(); it != varDecls.end();)
    {
        /*
        First check if code generation is disabled for variable declaration,
        then check if this is a system value semantic inside an interface block.
        */
        if ( (*it)->flags(VarDecl::disableCodeGen) ||
             ( isInsideInterfaceBlock_ && (*it)->semantic.IsSystemValue() ) )
        {
            /*
            Code generation is disabled for this variable declaration
            -> Remove this from the list
            */
            it = varDecls.erase(it);
        }
        else
            ++it;
    }

    if (varDecls.empty())
    {
        /*
        All variable declarations within this statement are disabled
        -> Break code generation here
        */
        return;
    }
    #endif

    BeginLn();

    /* Write input modifiers */
    if (ast->flags(VarDeclStmnt::isShaderInput))
        Write("in ");
    else if (ast->flags(VarDeclStmnt::isShaderOutput))
        Write("out ");

    /* Write storage classes */
    for (auto storageClass : ast->storageClasses)
    {
        auto keyword = StorageClassToGLSLKeyword(storageClass);
        if (keyword)
            Write(*keyword + " ");
        else
            Error("not all storage classes or interpolation modifiers can be mapped to GLSL keyword", ast);
    }

    /* Write type modifiers */
    for (const auto& modifier : ast->typeModifiers)
    {
        if (modifier == "const")
            Write(modifier + " ");
    }

    /* Write variable type */
    if (ast->varType->structDecl)
    {
        /* Do not end line here with "EndLn" */
        Visit(ast->varType);
        BeginLn();
    }
    else
    {
        Visit(ast->varType);
        Write(" ");
    }

    /* Write variable declarations */
    for (size_t i = 0; i < varDecls.size(); ++i)
    {
        Visit(varDecls[i]);
        if (i + 1 < varDecls.size())
            Write(", ");
    }

    Write(";");
    EndLn();
}

IMPLEMENT_VISIT_PROC(AliasDeclStmnt)
{
    if (ast->structDecl && !ast->structDecl->IsAnonymous())
    {
        Line(ast);

        bool semicolon = true;
        Visit(ast->structDecl, &semicolon);

        Blank();
    }
}

/* --- Statements --- */

IMPLEMENT_VISIT_PROC(NullStmnt)
{
    WriteLn(";");
}

IMPLEMENT_VISIT_PROC(CodeBlockStmnt)
{
    Visit(ast->codeBlock);
}

IMPLEMENT_VISIT_PROC(ForLoopStmnt)
{
    /* Write loop header */
    BeginLn();
    {
        Write("for (");

        PushOptions({ false, false });
        {
            Visit(ast->initSmnt);
            Write(" "); // initStmnt already has the ';'!
            Visit(ast->condition);
            Write("; ");
            Visit(ast->iteration);
        }
        PopOptions();

        Write(")");
    }
    EndLn();

    WriteScopedStmnt(ast->bodyStmnt.get());
}

IMPLEMENT_VISIT_PROC(WhileLoopStmnt)
{
    /* Write loop condExpr */
    BeginLn();
    {
        Write("while (");
        Visit(ast->condition);
        Write(")");
    }
    EndLn();

    WriteScopedStmnt(ast->bodyStmnt.get());
}

IMPLEMENT_VISIT_PROC(DoWhileLoopStmnt)
{
    WriteLn("do");
    WriteScopedStmnt(ast->bodyStmnt.get());

    /* Write loop condExpr */
    BeginLn();
    {
        Write("while (");
        Visit(ast->condition);
        Write(");");
    }
    EndLn();
}

IMPLEMENT_VISIT_PROC(IfStmnt)
{
    bool hasElseParentNode = (args != nullptr ? *reinterpret_cast<bool*>(&args) : false);

    /* Write if condExpr */
    if (!hasElseParentNode)
        BeginLn();
    
    Write("if (");
    Visit(ast->condition);
    Write(")");
    
    EndLn();

    /* Write if body */
    WriteScopedStmnt(ast->bodyStmnt.get());

    Visit(ast->elseStmnt);
}

IMPLEMENT_VISIT_PROC(ElseStmnt)
{
    if (ast->bodyStmnt->Type() == AST::Types::IfStmnt)
    {
        /* Write else if statement */
        BeginLn();
        Write("else ");

        bool hasElseParentNode = true;
        Visit(ast->bodyStmnt, &hasElseParentNode);
    }
    else
    {
        /* Write else statement */
        WriteLn("else");
        WriteScopedStmnt(ast->bodyStmnt.get());
    }
}

IMPLEMENT_VISIT_PROC(SwitchStmnt)
{
    /* Write selector */
    BeginLn();
    {
        Write("switch (");
        Visit(ast->selector);
        Write(")");
    }
    EndLn();

    /* Write switch cases */
    OpenScope();
    {
        Visit(ast->cases);
    }
    CloseScope();
}

IMPLEMENT_VISIT_PROC(ExprStmnt)
{
    BeginLn();
    {
        Visit(ast->expr);
        Write(";");
    }
    EndLn();
}

IMPLEMENT_VISIT_PROC(ReturnStmnt)
{
    if (isInsideEntryPoint_)
    {
        /* Write all output semantics assignment with the expression of the return statement */
        WriteOutputSemanticsAssignment(ast->expr.get());

        /* Is this return statement at the end of the function scope? */
        if (!ast->flags(ReturnStmnt::isEndOfFunction))
            WriteLn("return;");
    }
    else
    {
        if (ast->expr)
        {
            BeginLn();
            {
                Write("return ");
                Visit(ast->expr);
                Write(";");
            }
            EndLn();
        }
        else if (!ast->flags(ReturnStmnt::isEndOfFunction))
            WriteLn("return;");
    }
}

IMPLEMENT_VISIT_PROC(CtrlTransferStmnt)
{
    WriteLn(CtrlTransformToString(ast->transfer) + ";");
}

/* --- Expressions --- */

IMPLEMENT_VISIT_PROC(ListExpr)
{
    Visit(ast->firstExpr);
    Write(", ");
    Visit(ast->nextExpr);
}

IMPLEMENT_VISIT_PROC(LiteralExpr)
{
    Write(ast->value);
}

IMPLEMENT_VISIT_PROC(TypeNameExpr)
{
    WriteTypeDenoter(*ast->typeDenoter, ast);
}

IMPLEMENT_VISIT_PROC(TernaryExpr)
{
    Visit(ast->condExpr);
    Write(" ? ");
    Visit(ast->thenExpr);
    Write(" : ");
    Visit(ast->elseExpr);
}

IMPLEMENT_VISIT_PROC(BinaryExpr)
{
    Visit(ast->lhsExpr);
    Write(" " + BinaryOpToString(ast->op) + " ");
    Visit(ast->rhsExpr);
}

IMPLEMENT_VISIT_PROC(UnaryExpr)
{
    Write(UnaryOpToString(ast->op));
    Visit(ast->expr);
}

IMPLEMENT_VISIT_PROC(PostUnaryExpr)
{
    Visit(ast->expr);
    Write(UnaryOpToString(ast->op));
}

IMPLEMENT_VISIT_PROC(FunctionCallExpr)
{
    Visit(ast->call);
}

IMPLEMENT_VISIT_PROC(BracketExpr)
{
    Write("(");
    Visit(ast->expr);
    Write(")");
}

IMPLEMENT_VISIT_PROC(SuffixExpr)
{
    auto typeDenoter = ast->expr->GetTypeDenoter()->Get();

    /*
    First write all scalar swizzle operations as vector constructors,
    e.g. "1.0.xxxx" -> "vec4(1.0)", or "1.0.xx.y.xxx" -> "vec3(vec2(1.0).y)"
    */
    WriteSuffixVarIdentBegin(*typeDenoter, ast->varIdent.get());

    /* Write left-hand-side expression of suffix */
    Visit(ast->expr);

    /* Write suffix identifiers with optional vector constructor endings (i.e. closing ')' brackets) */
    WriteSuffixVarIdentEnd(*typeDenoter, ast->varIdent.get());
}

IMPLEMENT_VISIT_PROC(ArrayAccessExpr)
{
    Visit(ast->expr);
    WriteArrayDims(ast->arrayIndices);
}

IMPLEMENT_VISIT_PROC(CastExpr)
{
    Visit(ast->typeExpr);
    Write("(");
    Visit(ast->expr);
    Write(")");
}

IMPLEMENT_VISIT_PROC(VarAccessExpr)
{
    //WriteVarIdentOrSystemValue(ast->varIdent.get());
    Visit(ast->varIdent);
    if (ast->assignExpr)
    {
        Write(" " + AssignOpToString(ast->assignOp) + " ");
        Visit(ast->assignExpr);
    }
}

IMPLEMENT_VISIT_PROC(InitializerExpr)
{
    Write("{ ");
        
    for (size_t i = 0; i < ast->exprs.size(); ++i)
    {
        Visit(ast->exprs[i]);
        if (i + 1 < ast->exprs.size())
            Write(", ");
    }

    Write(" }");
}

#undef IMPLEMENT_VISIT_PROC

/* --- Helper functions for code generation --- */

/* --- Attribute --- */

void GLSLGenerator::WriteAttribute(Attribute* ast)
{
    if (ast->ident == "numthreads")
        WriteAttributeNumThreads(ast);
    else if (ast->ident == "earlydepthstencil")
        WriteAttributeEarlyDepthStencil();
}

void GLSLGenerator::WriteAttributeNumThreads(Attribute* ast)
{
    if (ast->arguments.size() == 3)
    {
        BeginLn();
        {
            Write("layout(local_size_x = ");
            Visit(ast->arguments[0]);

            Write(", local_size_y = ");
            Visit(ast->arguments[1]);

            Write(", local_size_z = ");
            Visit(ast->arguments[2]);

            Write(") in;");
        }
        EndLn();
    }
    else
        ErrorInvalidNumArgs("\"numthreads\" attribute", ast);
}

void GLSLGenerator::WriteAttributeEarlyDepthStencil()
{
    WriteLn("layout(early_fragment_tests) in;");
}

/* --- Input semantics --- */

void GLSLGenerator::WriteLocalInputSemantics()
{
    auto& varDeclRefs = GetProgram()->entryPointRef->inputSemantics.varDeclRefsSV;

    bool paramsWritten = false;

    for (auto varDecl : varDeclRefs)
    {
        if (WriteLocalInputSemanticsVarDecl(varDecl))
            paramsWritten = true;
    }

    if (paramsWritten)
        Blank();
}

bool GLSLGenerator::WriteLocalInputSemanticsVarDecl(VarDecl* varDecl)
{
    /* Is semantic of the variable declaration a system value semantic? */
    if (varDecl->semantic.IsValid())
    {
        if (auto semanticKeyword = SemanticToGLSLKeyword(varDecl->semantic))
        {
            /* Write local variable definition statement */
            BeginLn();
            {
                Visit(varDecl->declStmntRef->varType);
                Write(" " + varDecl->ident + " = " + *semanticKeyword + ";");
            }
            EndLn();
        }
        else
            Error("failed to map semantic name to GLSL keyword", varDecl);

        return true;
    }
    return false;
}

void GLSLGenerator::WriteGlobalInputSemantics()
{
    auto& varDeclRefs = GetProgram()->entryPointRef->inputSemantics.varDeclRefs;

    bool paramsWritten = false;

    for (auto varDecl : varDeclRefs)
    {
        if (WriteGlobalInputSemanticsVarDecl(varDecl))
            paramsWritten = true;
    }

    if (paramsWritten)
        Blank();
}

bool GLSLGenerator::WriteGlobalInputSemanticsVarDecl(VarDecl* varDecl)
{
    /* Write global variable definition statement */
    BeginLn();
    {
        Write("in ");
        Visit(varDecl->declStmntRef->varType);
        Write(" " + varDecl->ident + ";");
    }
    EndLn();

    return true;
}

/* --- Output semantics --- */

void GLSLGenerator::WriteLocalOutputSemantics()
{
    auto& varDeclRefs = GetProgram()->entryPointRef->outputSemantics.varDeclRefsSV;

    bool paramsWritten = false;

    for (auto varDecl : varDeclRefs)
    {
        if (WriteLocalOutputSemanticsVarDecl(varDecl))
            paramsWritten = true;
    }

    if (paramsWritten)
        Blank();
}

bool GLSLGenerator::WriteLocalOutputSemanticsVarDecl(VarDecl* varDecl)
{
    /* Write local variable definition statement (without initialization) */
    BeginLn();
    {
        Visit(varDecl->declStmntRef->varType);
        Write(" " + varDecl->ident + ";");
    }
    EndLn();
    return true;
}

void GLSLGenerator::WriteGlobalOutputSemantics()
{
    auto& varDeclRefs = GetProgram()->entryPointRef->outputSemantics.varDeclRefs;

    bool paramsWritten = false;

    for (auto varDecl : varDeclRefs)
    {
        if (WriteGlobalOutputSemanticsVarDecl(varDecl))
            paramsWritten = true;
    }

    if (paramsWritten)
        Blank();
}

bool GLSLGenerator::WriteGlobalOutputSemanticsVarDecl(VarDecl* varDecl)
{
    /* Write global variable definition statement */
    BeginLn();
    {
        if (varDecl->semantic.IsValid())
            Write("layout(location = " + std::to_string(varDecl->semantic.Index()) + ") out ");
        else
            Write("out ");

        Visit(varDecl->declStmntRef->varType);

        Write(" " + varDecl->ident + ";");
    }
    EndLn();

    return true;
}

void GLSLGenerator::WriteOutputSemanticsAssignment(Expr* ast)
{
    auto        entryPoint  = GetProgram()->entryPointRef;
    auto        semantic    = entryPoint->semantic;
    const auto& varDeclRefs = entryPoint->outputSemantics.varDeclRefsSV;

    /* Prefer variables that are system semantics */
    if (!varDeclRefs.empty())
    {
        /* Write system values */
        for (auto varDecl : varDeclRefs)
        {
            if (varDecl->semantic.IsValid())
            {
                if (auto semanticKeyword = SemanticToGLSLKeyword(varDecl->semantic))
                {
                    BeginLn();
                    {
                        Write(*semanticKeyword + " = " + varDecl->ident + ";");
                    }
                    EndLn();
                }
            }
        }
    }
    else if (semantic.IsSystemValue() && ast)
    {
        if (auto semanticKeyword = SemanticToGLSLKeyword(semantic))
        {
            BeginLn();
            {
                Write(*semanticKeyword);
                Write(" = ");
                Visit(ast);
                Write(";");
            }
            EndLn();
        }
        else
            Error("failed to map output semantic to GLSL keyword", entryPoint);
    }
    else if (shaderTarget_ != ShaderTarget::ComputeShader)
        Error("missing output semantic", ast);
}

/* --- VarIdent --- */

const std::string& GLSLGenerator::FinalIdentFromVarIdent(VarIdent* ast)
{
    /* Check if a variable declaration has changed it's name during conversion */
    if (ast->symbolRef)
    {
        if (auto varDecl = ast->symbolRef->As<VarDecl>())
            return varDecl->ident;
    }

    /* Return default identifier */
    return ast->ident;
}

void GLSLGenerator::WriteVarIdent(VarIdent* ast, bool recursive)
{
    /* Write identifier */
    Write(FinalIdentFromVarIdent(ast));

    /* Write array index expressions */
    WriteArrayDims(ast->arrayIndices);

    if (recursive && ast->next)
    {
        Write(".");
        WriteVarIdent(ast->next.get());
    }
}

static TypeDenoterPtr GetTypeDenoterForSuffixVarIdent(const TypeDenoter& lhsTypeDen, VarIdent* ast)
{
    if (lhsTypeDen.IsBase())
    {
        /* Get type denoter from vector subscript */
        auto lhsBaseTypeDen = lhsTypeDen.As<BaseTypeDenoter>();
        auto subscriptDataType = SubscriptDataType(lhsBaseTypeDen->dataType, ast->ident);
        return std::make_shared<BaseTypeDenoter>(subscriptDataType);
    }
    else
    {
        /* Get type denoter from symbol reference (in VarIdent) */
        return ast->GetExplicitTypeDenoter(false)->Get();
    }
}

void GLSLGenerator::WriteSuffixVarIdentBegin(const TypeDenoter& lhsTypeDen, VarIdent* ast)
{
    /* First traverse sub nodes */
    if (ast->next)
    {
        /* Get type denoter of current VarIdent AST node */
        auto typeDenoter = GetTypeDenoterForSuffixVarIdent(lhsTypeDen, ast);
        WriteSuffixVarIdentBegin(*typeDenoter, ast->next.get());
    }

    /* Has this node a scalar type? */
    if (lhsTypeDen.IsScalar())
    {
        auto lhsBaseTypeDen = lhsTypeDen.As<BaseTypeDenoter>();
        WriteDataType(SubscriptDataType(lhsBaseTypeDen->dataType, ast->ident), ast);
        Write("(");
    }
}

void GLSLGenerator::WriteSuffixVarIdentEnd(const TypeDenoter& lhsTypeDen, VarIdent* ast)
{
    /* First write identifier */
    if (lhsTypeDen.IsScalar())
    {
        /* Close vector constructor */
        Write(")");
    }
    else
    {
        /* Write next identifier */
        Write(".");
        WriteVarIdent(ast, false);
    }

    /* Now traverse sub nodes */
    if (ast->next)
    {
        auto typeDenoter = GetTypeDenoterForSuffixVarIdent(lhsTypeDen, ast);
        WriteSuffixVarIdentEnd(*typeDenoter, ast->next.get());
    }
}

/* --- Type denoter --- */

void GLSLGenerator::WriteDataType(DataType dataType, const AST* ast)
{
    /* Replace doubles with floats, if doubles are not supported */
    if (versionOut_ < OutputShaderVersion::GLSL400)
        dataType = DoubleToFloatDataType(dataType);

    /* Map GLSL data type */
    if (auto keyword = DataTypeToGLSLKeyword(dataType))
        Write(*keyword);
    else
        Error("failed to map data type to GLSL keyword", ast);
}

void GLSLGenerator::WriteTypeDenoter(const TypeDenoter& typeDenoter, const AST* ast)
{
    try
    {
        if (typeDenoter.IsVoid())
        {
            /* Just write void type */
            Write("void");
        }
        else if (auto baseTypeDen = typeDenoter.As<BaseTypeDenoter>())
        {
            /* Map GLSL base type */
            WriteDataType(baseTypeDen->dataType, ast);
        }
        else if (auto textureTypeDen = typeDenoter.As<TextureTypeDenoter>())
        {
            /* Get texture type */
            auto textureType = textureTypeDen->textureType;
            if (textureType == BufferType::Undefined)
            {
                if (auto texDecl = textureTypeDen->textureDeclRef)
                    textureType = texDecl->declStmntRef->textureType;
                else
                    Error("missing reference to texture type denoter", ast);
            }

            /* Convert texture type to GLSL sampler type */
            if (auto keyword = BufferTypeToGLSLKeyword(textureType))
                Write(*keyword);
            else
                Error("failed to map texture type to GLSL keyword", ast);
        }
        else if (typeDenoter.IsStruct())
        {
            /* Write struct identifier */
            Write(typeDenoter.Ident());
        }
        else if (typeDenoter.IsAlias())
        {
            /* Write aliased type denoter */
            WriteTypeDenoter(typeDenoter.GetAliased(), ast);
        }
        else if (auto arrayTypeDen = typeDenoter.As<ArrayTypeDenoter>())
        {
            /* Write array type denoter */
            WriteTypeDenoter(*arrayTypeDen->baseTypeDenoter, ast);
            WriteArrayDims(arrayTypeDen->arrayDims);
        }
        else
            Error("failed to determine GLSL data type", ast);
    }
    catch (const Report& e)
    {
        throw e;
    }
    catch (const std::exception& e)
    {
        Error(e.what(), ast);
    }
}

/* --- Function call --- */

void GLSLGenerator::AssertIntrinsicNumArgs(FunctionCall* ast, std::size_t numArgsMin, std::size_t numArgsMax)
{
    if (ast->arguments.size() < numArgsMin || ast->arguments.size() > numArgsMax)
        Error("invalid number of arguments in intrinsic", ast);
}

void GLSLGenerator::WriteFunctionCallStandard(FunctionCall* ast)
{
    /* Write function name */
    if (ast->varIdent)
    {
        if (ast->intrinsic != Intrinsic::Undefined)
        {
            /* Write GLSL intrinsic keyword */
            auto keyword = IntrinsicToGLSLKeyword(ast->intrinsic);
            if (keyword)
                Write(*keyword);
            else
                Error("failed to map intrinsic '" + ast->varIdent->LastVarIdent()->ToString() + "' to GLSL keyword", ast);
        }
        else
        {
            /* Write function identifier */
            Visit(ast->varIdent);
        }
    }
    else if (ast->typeDenoter)
    {
        /* Write type denoter */
        WriteTypeDenoter(*ast->typeDenoter, ast);
    }
    else
        Error("missing function name", ast);

    /* Write arguments */
    Write("(");

    for (size_t i = 0; i < ast->arguments.size(); ++i)
    {
        Visit(ast->arguments[i]);
        if (i + 1 < ast->arguments.size())
            Write(", ");
    }

    Write(")");
}

void GLSLGenerator::WriteFunctionCallIntrinsicMul(FunctionCall* ast)
{
    AssertIntrinsicNumArgs(ast, 2, 2);

    auto WriteMulArgument = [&](const ExprPtr& expr)
    {
        /*
        Determine if the expression needs extra brackets when converted from a function call "mul(lhs, rhs)" to a binary expression "lhs * rhs",
        e.g. "mul(wMatrix, pos + float4(0, 1, 0, 0))" -> "wMatrix * (pos + float4(0, 1, 0, 0))" needs extra brackets
        */
        auto type = expr->Type();
        if (type == AST::Types::TernaryExpr || type == AST::Types::BinaryExpr || type == AST::Types::UnaryExpr || type == AST::Types::PostUnaryExpr)
        {
            Write("(");
            Visit(expr);
            Write(")");
        }
        else
            Visit(expr);
    };

    /* Convert this function call into a multiplication */
    Write("(");
    {
        WriteMulArgument(ast->arguments[0]);
        Write(" * ");
        WriteMulArgument(ast->arguments[1]);
    }
    Write(")");
}

void GLSLGenerator::WriteFunctionCallIntrinsicRcp(FunctionCall* ast)
{
    AssertIntrinsicNumArgs(ast, 1, 1);

    /* Get type denoter of argument expression */
    auto& expr = ast->arguments.front();
    auto typeDenoter = expr->GetTypeDenoter()->Get();

    if (typeDenoter->IsBase())
    {
        /* Convert this function call into a division */
        Write("(");
        {
            WriteTypeDenoter(*typeDenoter, ast);
            Write("(1) / (");
            Visit(expr);
        }
        Write("))");
    }
    else
        Error("invalid argument type for intrinsic 'rcp'", expr.get());
}

void GLSLGenerator::WriteFunctionCallIntrinsicAtomic(FunctionCall* ast)
{
    AssertIntrinsicNumArgs(ast, 2, 3);

    //TODO: move this to another visitor (e.g. "GLSLConverter" or the like) which does some transformation on the AST
    /* Find atomic intrinsic mapping */
    auto keyword = IntrinsicToGLSLKeyword(ast->intrinsic);
    if (keyword)
    {
        /* Write function call */
        if (ast->arguments.size() >= 3)
        {
            Visit(ast->arguments[2]);
            Write(" = ");
        }
        Write(*keyword + "(");
        Visit(ast->arguments[0]);
        Write(", ");
        Visit(ast->arguments[1]);
        Write(")");
    }
    else
        Error("failed to map intrinsic '" + ast->varIdent->ToString() + "' to GLSL keyword", ast);
}

/* --- Structure --- */

void GLSLGenerator::WriteStructDecl(StructDecl* ast, bool writeSemicolon, bool allowNestedStruct)
{
    /* Is this a non-nested structure or are nested structures allowed in the current context? */
    if (!ast->flags(StructDecl::isNestedStruct) || allowNestedStruct)
    {
        /* Is this an interface block or a standard structure? */
        if (ast->flags(StructDecl::isShaderInput) || ast->flags(StructDecl::isShaderOutput))
        {
            /* Write this structure as interface block (if structure doesn't need to be resolved) */
            BeginLn();
            {
                if (ast->flags(StructDecl::isShaderInput))
                    Write("in ");
                else
                    Write("out ");
                Write(ast->ident);
            }
            EndLn();

            OpenScope();
            {
                isInsideInterfaceBlock_ = true;

                Visit(ast->members);

                isInsideInterfaceBlock_ = false;
            }
            CloseScope();

            WriteLn(ast->aliasName + ";");
        }
        else
        {
            /* Write standard structure declaration */
            BeginLn();
            {
                Write("struct");
                if (!ast->ident.empty())
                    Write(' ' + ast->ident);
            }
            EndLn();

            OpenScope();
            {
                WriteStructDeclMembers(ast);
            }
            CloseScope(writeSemicolon);
        }
    }
    else if (!writeSemicolon)
    {
        BeginLn();
        Write(ast->ident + " ");
        /* Do not end line here with "EndLn" */
    }
}

void GLSLGenerator::WriteStructDeclMembers(StructDecl* ast)
{
    if (ast->baseStructRef)
        WriteStructDeclMembers(ast->baseStructRef);
    Visit(ast->members);
}

/* --- Misc --- */

void GLSLGenerator::WriteParameter(VarDeclStmnt* ast)
{
    /* Write modifiers */
    if (!ast->inputModifier.empty())
        Write(ast->inputModifier + " ");

    for (const auto& modifier : ast->typeModifiers)
    {
        if (modifier == "const")
            Write(modifier + " ");
    }

    /* Write parameter type */
    Visit(ast->varType);
    Write(" ");

    /* Write parameter identifier */
    if (ast->varDecls.size() == 1)
        Visit(ast->varDecls.front());
    else
        Error("invalid number of variables in function parameter", ast);
}

void GLSLGenerator::WriteScopedStmnt(Stmnt* ast)
{
    if (ast)
    {
        if (ast->Type() != AST::Types::CodeBlockStmnt)
        {
            IncIndent();
            Visit(ast);
            DecIndent();
        }
        else
            Visit(ast);
    }
}

void GLSLGenerator::WriteArrayDims(const std::vector<ExprPtr>& arrayDims)
{
    for (auto& dim : arrayDims)
    {
        Write("[");
        Visit(dim);
        Write("]");
    }
}


} // /namespace Xsc



// ================================================================================
