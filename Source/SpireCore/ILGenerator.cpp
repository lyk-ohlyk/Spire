#include "Syntax.h"
#include "ScopeDictionary.h"
#include "CodeWriter.h"
#include "Naming.h"

namespace Spire
{
    namespace Compiler
    {
        class ILGenerator : public SyntaxVisitor
        {
        public:
            ILProgram * program = nullptr;
			CompileOptions compileOptions;
			HashSet<String> entryPointNames;
            ILGenerator(ILProgram * result, DiagnosticSink * sink, CompileOptions options)
                : SyntaxVisitor(sink), compileOptions(options)
            {
                program = result;
				for (auto && entry : options.entryPoints)
					entryPointNames.Add(entry.name);
            }
        private:
            Dictionary<DeclRef, RefPtr<ILStructType>> structTypes;
            Dictionary<DeclRef, ILFunction*> functions;
            ScopeDictionary<String, ILOperand*> variables;
            CodeWriter codeWriter;
        private:
            RefPtr<ILStructType> TranslateStructType(AggTypeDecl* structDecl)
            {
                RefPtr<ILStructType> ilStructType;

                if (structTypes.TryGetValue(DeclRef(structDecl, nullptr), ilStructType))
                {
                    return ilStructType;
                }

                ilStructType = new ILStructType();
                ilStructType->TypeName = structDecl->Name.Content;

                for (auto field : structDecl->GetMembersOfType<StructField>())
                {
                    ILStructType::ILStructField ilField;
                    ilField.FieldName = field->Name.Content;
                    ilField.Type = TranslateExpressionType(field->Type.Ptr());
                    ilStructType->Members.Add(ilField);
                }

                structTypes.Add(DeclRef(structDecl, nullptr), ilStructType);
                return ilStructType;
            }

            int GetIntVal(RefPtr<IntVal> val)
            {
                if (auto constantVal = val.As<ConstantIntVal>())
                {
                    return constantVal->value;
                }
                assert(!"unexpected");
                return 0;
            }

            RefPtr<ILType> TranslateExpressionType(ExpressionType * type)
            {
                if (auto basicType = type->AsBasicType())
                {
                    auto base = new ILBasicType();
                    base->Type = (ILBaseType)basicType->BaseType;
                    return base;
                }
                else if (auto vecType = type->AsVectorType())
                {
                    auto elementType = vecType->elementType->AsBasicType();
                    int elementCount = GetIntVal(vecType->elementCount);
                    assert(elementType);
                    return new ILVectorType((ILBaseType)elementType->BaseType, elementCount);
                }
                else if (auto matType = type->AsMatrixType())
                {
                    auto elementType = matType->elementType->AsBasicType();
                    int rowCount = GetIntVal(matType->rowCount);
                    int colCount = GetIntVal(matType->colCount);
                    assert(elementType);
                    return new ILMatrixType((ILBaseType)elementType->BaseType, rowCount, colCount);
                }
                else if (auto texType = type->As<TextureType>())
                {
                    return new ILTextureType(TranslateExpressionType(texType->elementType.Ptr()),
                        (ILTextureShape)texType->GetBaseShape(),
                        texType->isMultisample(),
                        texType->IsArray(),
                        texType->isShadow());
                }
                else if (auto cbufferType = type->As<ConstantBufferType>())
                {
                    auto ilType = new ILPointerLikeType(ILPointerLikeTypeName::ConstantBuffer, TranslateExpressionType(cbufferType->elementType.Ptr()));
                    return ilType;
                }
                else if (auto declRefType = type->AsDeclRefType())
                {
                    auto decl = declRefType->declRef.decl;
                    if (auto structDecl = dynamic_cast<StructSyntaxNode*>(decl))
                    {
                        return TranslateStructType(structDecl);
                    }
                    else
                    {
                        throw NotImplementedException("decl type");
                    }
                }
                else if (auto arrType = type->AsArrayType())
                {
                    auto nArrType = new ILArrayType();
                    nArrType->BaseType = TranslateExpressionType(arrType->BaseType.Ptr());
                    nArrType->ArrayLength = arrType->ArrayLength ? GetIntVal(arrType->ArrayLength) : 0;
                    return nArrType;
                }
                throw NotImplementedException("decl type");
            }

            RefPtr<ILType> TranslateExpressionType(const RefPtr<ExpressionType> & type)
            {
                return TranslateExpressionType(type.Ptr());
            }

            ParameterQualifier GetParamDirectionQualifier(ParameterSyntaxNode* paramDecl)
            {
                if (paramDecl->HasModifier<InOutModifier>())
                    return ParameterQualifier::InOut;
                else if (paramDecl->HasModifier<OutModifier>())
                    return ParameterQualifier::Out;
                else
                    return ParameterQualifier::In;
            }
            
            FetchArgInstruction * thisArg = nullptr;
            DeclRef thisDeclRef;
            void GenerateFunctionHeader(FunctionSyntaxNode * f, ILStructType * thisType)
            {
                RefPtr<ILFunction> func = new ILFunction();
                StringBuilder internalName;
                if (thisType)
                    internalName << thisType->TypeName << "@";
                internalName << f->Name.Content;
                for (auto & para : f->GetParameters())
                {
                    internalName << "@" << para->Type.type->ToString();
                }
                f->InternalName = internalName.ProduceString();
                program->Functions.Add(f->InternalName, func);
                func->Name = f->InternalName;
                func->ReturnType = TranslateExpressionType(f->ReturnType);
                
                functions[DeclRef(f, nullptr)] = func.Ptr();
            }
            ILFunction * GenerateFunction(FunctionSyntaxNode * f, ILStructType * thisType)
            {
                RefPtr<ILFunction> func = functions[DeclRef(f, nullptr)]();
                variables.PushScope();
                codeWriter.PushNode();
                int id = 0;
                if (thisType)
                {
                    thisArg = codeWriter.FetchArg(thisType, ++id, ParameterQualifier::InOut);
                    func->Parameters.Add("this", thisArg);
                    thisArg->Name = "sv_this";
                    variables.Add("this", thisArg);
                }
                for (auto &param : f->GetParameters())
                {
                    auto op = codeWriter.FetchArg(TranslateExpressionType(param->Type.Ptr()), ++id, GetParamDirectionQualifier(param.Ptr()));
                    func->Parameters.Add(param->Name.Content, op);
                    op->Name = EscapeCodeName(String("p_") + param->Name.Content);
                    variables.Add(param->Name.Content, op);
                }
                f->Body->Accept(this);
                func->Code = codeWriter.PopNode();
                variables.PopScope();
                thisArg = nullptr;
				return func.Ptr();
            }
            void GenerateMemberFunctionHeader(ClassSyntaxNode * node)
            {
                thisDeclRef = DeclRef(node, nullptr);
                auto thisType = structTypes[thisDeclRef]();
                for (auto && f : node->GetMembersOfType<FunctionSyntaxNode>())
                {
                    GenerateFunctionHeader(f.Ptr(), thisType.Ptr());
                }
                thisDeclRef = DeclRef();
            }
            void GenerateMemberFunction(ClassSyntaxNode * node)
            {
                thisDeclRef = DeclRef(node, nullptr);
                auto thisType = structTypes[thisDeclRef]();
                for (auto && f : node->GetMembersOfType<FunctionSyntaxNode>())
                {
                    GenerateFunction(f.Ptr(), thisType.Ptr());
                }
                thisDeclRef = DeclRef();
            }
			void DefineBindableResourceVariables(ILOperand * obj, String namePrefix)
			{
				auto structType = obj->Type.As<ILStructType>();
				assert(structType);
				int memberIndex = 0;
				for (auto field : structType->Members)
				{
					if (field.Type->GetBindableResourceType() != BindableResourceType::NonBindable)
					{
						// create global variable
						auto gvar = new ILGlobalVariable(field.Type);
						gvar->Name = namePrefix + "_" + field.FieldName;
						program->GlobalVars.Add(gvar->Name, gvar);
						variables.Add(gvar->Name, gvar);
						//TODO: assign bindings here

						// insert assignment instruction in init func
						GenerateIndexExpression(obj, program->ConstantPool->CreateConstant(memberIndex));
						codeWriter.Assign(PopStack(), gvar);
					}
					else if (field.Type.As<ILStructType>())
					{
						GenerateIndexExpression(obj, program->ConstantPool->CreateConstant(memberIndex));
						DefineBindableResourceVariables(PopStack(), namePrefix + "_" + field.FieldName);
					}
					memberIndex++;
				}
			}
        public:
            virtual RefPtr<ProgramSyntaxNode> VisitProgram(ProgramSyntaxNode * prog) override
            {
                for (auto&& s : prog->GetStructs())
                {
                    if (s->HasModifier<IntrinsicModifier>() || s->HasModifier<FromStdLibModifier>())
                        continue;
                    s->Accept(this);
                }
                auto classes = prog->GetMembersOfType<ClassSyntaxNode>();
                for (auto&& c : classes)
                {
                    TranslateStructType(c.Ptr());
                }
                for (auto&& c : classes)
                {
                    GenerateMemberFunctionHeader(c.Ptr());
                }
                variables.PushScope();

				RefPtr<ILFunction> initFunc = new ILFunction();
				initFunc->Name = "__main_init";
				initFunc->ReturnType = new ILBasicType(ILBaseType::Void);
				codeWriter.PushNode();
                for (auto&& v : prog->GetMembersOfType<Variable>())
                {
                    if (v->HasModifier<IntrinsicModifier>() || v->HasModifier<FromStdLibModifier>())
                        continue;
                    if (v->Type->IsClass())
                    {
                        auto declRef = v->Type->AsDeclRefType()->declRef;
                        auto structType = structTypes[declRef]();
                        ILGlobalVariable * gvar = new ILGlobalVariable(structType.Ptr());
                        gvar->Name = v->Name.Content;
                        gvar->Position = v->Position;
						variables.Add(gvar->Name, gvar);
                        DefineBindableResourceVariables(gvar, gvar->Name);
						program->GlobalVars.Add(gvar->Name, gvar);
                    }
                    else
						v->Accept(this);
                }
				initFunc->Code = codeWriter.PopNode();
				program->Functions.Add(initFunc->Name, initFunc);

                for (auto&& f : prog->GetFunctions())
                {
                    if (f->HasModifier<IntrinsicModifier>() || f->HasModifier<FromStdLibModifier>())
                        continue;
                    GenerateFunctionHeader(f.Ptr(), nullptr);
                }

                for (auto&& c : classes)
                {
                    GenerateMemberFunction(c.Ptr());
                }
                for (auto&& f : prog->GetFunctions())
                {
                    if (f->HasModifier<IntrinsicModifier>() || f->HasModifier<FromStdLibModifier>())
                        continue;
                    auto func = GenerateFunction(f.Ptr(), nullptr);
					if (entryPointNames.Contains(f->Name.Content))
					{
						auto call = new CallInstruction(0);
						call->Type = initFunc->ReturnType;
						call->Function = initFunc->Name;
						func->Code->InsertHead(call);
					}
                }
                variables.PopScope();
                return prog;
            }
            virtual RefPtr<StructSyntaxNode> VisitStruct(StructSyntaxNode * st) override
            {
                RefPtr<ILStructType> structType = TranslateStructType(st);
                program->Structs.Add(structType);
                return st;
            }
            virtual RefPtr<FunctionSyntaxNode> VisitFunction(FunctionSyntaxNode* function) override
            {
                if (function->IsExtern())
                    return function;
                GenerateFunction(function, nullptr);
                return function;
            }

        public:
            // functions for emiting code body (statements and expressions)
            ILOperand * exprStack = nullptr;
            ILOperand * returnRegister = nullptr;
            void PushStack(ILOperand * op)
            {
                assert(exprStack == nullptr);
                exprStack = op;
            }
            ILOperand * PopStack()
            {
                auto rs = exprStack;
                exprStack = nullptr;
                return rs;
            }
            virtual RefPtr<StatementSyntaxNode> VisitBlockStatement(BlockStatementSyntaxNode* stmt) override
            {
                variables.PushScope();
                for (auto & subStmt : stmt->Statements)
                    subStmt->Accept(this);
                variables.PopScope();
                return stmt;
            }
            virtual RefPtr<StatementSyntaxNode> VisitWhileStatement(WhileStatementSyntaxNode* stmt) override
            {
                RefPtr<WhileInstruction> instr = new WhileInstruction();
                variables.PushScope();
                codeWriter.PushNode();
                stmt->Predicate->Accept(this);
                codeWriter.Insert(new ReturnInstruction(PopStack()));
                instr->ConditionCode = codeWriter.PopNode();
                codeWriter.PushNode();
                stmt->Statement->Accept(this);
                instr->BodyCode = codeWriter.PopNode();
                codeWriter.Insert(instr.Release());
                variables.PopScope();
                return stmt;
            }
            virtual RefPtr<StatementSyntaxNode> VisitDoWhileStatement(DoWhileStatementSyntaxNode* stmt) override
            {
                RefPtr<DoInstruction> instr = new DoInstruction();
                variables.PushScope();
                codeWriter.PushNode();
                stmt->Predicate->Accept(this);
                codeWriter.Insert(new ReturnInstruction(PopStack()));
                instr->ConditionCode = codeWriter.PopNode();
                codeWriter.PushNode();
                stmt->Statement->Accept(this);
                instr->BodyCode = codeWriter.PopNode();
                codeWriter.Insert(instr.Release());
                variables.PopScope();
                return stmt;
            }
            virtual RefPtr<StatementSyntaxNode> VisitForStatement(ForStatementSyntaxNode* stmt) override
            {
                RefPtr<ForInstruction> instr = new ForInstruction();
                variables.PushScope();
                if (auto initStmt = stmt->InitialStatement.Ptr())
                {
                    // TODO(tfoley): any of this push-pop malarky needed here?
                    initStmt->Accept(this);
                }
                if (stmt->PredicateExpression)
                {
                    codeWriter.PushNode();
                    stmt->PredicateExpression->Accept(this);
                    PopStack();
                    instr->ConditionCode = codeWriter.PopNode();
                }

                if (stmt->SideEffectExpression)
                {
                    codeWriter.PushNode();
                    stmt->SideEffectExpression->Accept(this);
                    PopStack();
                    instr->SideEffectCode = codeWriter.PopNode();
                }

                codeWriter.PushNode();
                stmt->Statement->Accept(this);
                instr->BodyCode = codeWriter.PopNode();
                codeWriter.Insert(instr.Release());
                variables.PopScope();
                return stmt;
            }
            virtual RefPtr<StatementSyntaxNode> VisitIfStatement(IfStatementSyntaxNode* stmt) override
            {
                RefPtr<IfInstruction> instr = new IfInstruction();
                variables.PushScope();
                stmt->Predicate->Accept(this);
                instr->Operand = PopStack();
                codeWriter.PushNode();
                stmt->PositiveStatement->Accept(this);
                instr->TrueCode = codeWriter.PopNode();
                if (stmt->NegativeStatement)
                {
                    codeWriter.PushNode();
                    stmt->NegativeStatement->Accept(this);
                    instr->FalseCode = codeWriter.PopNode();
                }
                codeWriter.Insert(instr.Release());
                variables.PopScope();
                return stmt;
            }
            virtual RefPtr<StatementSyntaxNode> VisitReturnStatement(ReturnStatementSyntaxNode* stmt) override
            {
                returnRegister = nullptr;
                if (stmt->Expression)
                {
                    stmt->Expression->Accept(this);
                    returnRegister = PopStack();
                }
                codeWriter.Insert(new ReturnInstruction(returnRegister));
                return stmt;
            }
            virtual RefPtr<StatementSyntaxNode> VisitBreakStatement(BreakStatementSyntaxNode* stmt) override
            {
                codeWriter.Insert(new BreakInstruction());
                return stmt;
            }
            virtual RefPtr<StatementSyntaxNode> VisitContinueStatement(ContinueStatementSyntaxNode* stmt) override
            {
                codeWriter.Insert(new ContinueInstruction());
                return stmt;
            }
            virtual RefPtr<ExpressionSyntaxNode> VisitSelectExpression(SelectExpressionSyntaxNode * expr) override
            {
                expr->SelectorExpr->Accept(this);
                auto predOp = PopStack();
                expr->Expr0->Accept(this);
                auto v0 = PopStack();
                expr->Expr1->Accept(this);
                auto v1 = PopStack();
                PushStack(codeWriter.Select(predOp, v0, v1));
                return expr;
            }
            ILOperand * EnsureBoolType(ILOperand * op, RefPtr<ExpressionType> type)
            {
                if (!type->Equals(ExpressionType::Bool.Ptr()))
                {
                    auto cmpeq = new CmpneqInstruction();
                    cmpeq->Operands[0] = op;
                    cmpeq->Operands[1] = program->ConstantPool->CreateConstant(0);
                    cmpeq->Type = new ILBasicType(ILBaseType::Int);
                    codeWriter.Insert(cmpeq);
                    return cmpeq;
                }
                else
                    return op;
            }
            virtual RefPtr<StatementSyntaxNode> VisitDiscardStatement(DiscardStatementSyntaxNode * stmt) override
            {
                codeWriter.Discard();
                return stmt;
            }
            AllocVarInstruction * AllocVar(String name, ExpressionType * etype, CodePosition pos)
            {
                AllocVarInstruction * varOp = 0;
                RefPtr<ILType> type = TranslateExpressionType(etype);
                assert(type);
                if (codeWriter.GetCurrentNode())
                    varOp = codeWriter.AllocVar(type);
                else
                {
                    auto gvar = new ILGlobalVariable(type);
                    gvar->IsConst = false;
                    varOp = gvar;
                    program->GlobalVars[name] = gvar;
                }
                varOp->Name = name;
                varOp->Position = pos;
                return varOp;
            }

            RefPtr<Variable> VisitDeclrVariable(Variable* varDecl)
            {
                AllocVarInstruction * varOp = AllocVar(EscapeCodeName(varDecl->Name.Content), varDecl->Type.Ptr(), varDecl->Position);
                variables.Add(varDecl->Name.Content, varOp);
                if (varDecl->Expr)
                {
                    varDecl->Expr->Accept(this);
                    Assign(varOp, PopStack());
                }
                return varDecl;
            }

            virtual RefPtr<StatementSyntaxNode> VisitExpressionStatement(ExpressionStatementSyntaxNode* stmt) override
            {
                stmt->Expression->Accept(this);
                PopStack();
                return stmt;
            }
            void Assign(ILOperand * left, ILOperand * right)
            {
                codeWriter.Store(left, right);
            }
            virtual RefPtr<ExpressionSyntaxNode> VisitConstantExpression(ConstantExpressionSyntaxNode* expr) override
            {
                ILConstOperand * op;
                if (expr->ConstType == ConstantExpressionSyntaxNode::ConstantType::Float)
                {
                    op = program->ConstantPool->CreateConstant(expr->FloatValue);
                }
                else if (expr->ConstType == ConstantExpressionSyntaxNode::ConstantType::Bool)
                {
                    op = program->ConstantPool->CreateConstant(expr->IntValue != 0);
                }
                else
                {
                    op = program->ConstantPool->CreateConstant(expr->IntValue);
                }
                PushStack(op);
                return expr;
            }
            void GenerateIndexExpression(ILOperand * base, ILOperand * idx)
            {
                auto ldInstr = codeWriter.MemberAccess(base, idx);
                ldInstr->Attribute = base->Attribute;
                PushStack(ldInstr);
            }
            virtual RefPtr<ExpressionSyntaxNode> VisitIndexExpression(IndexExpressionSyntaxNode* expr) override
            {
                expr->BaseExpression->Access = expr->Access;
                expr->BaseExpression->Accept(this);
                auto base = PopStack();
                expr->IndexExpression->Access = ExpressionAccess::Read;
                expr->IndexExpression->Accept(this);
                auto idx = PopStack();
                GenerateIndexExpression(base, idx);
                return expr;
            }
            virtual RefPtr<ExpressionSyntaxNode> VisitSwizzleExpression(SwizzleExpr * expr) override
            {
                RefPtr<Object> refObj;
                expr->base->Access = expr->Access;
                expr->base->Accept(this);
                auto base = PopStack();
                StringBuilder swizzleStr;
                for (int i = 0; i < expr->elementCount; i++)
                    swizzleStr << (char)('x' + i);
                auto rs = new SwizzleInstruction();
                rs->Type = TranslateExpressionType(expr->Type.Ptr());
                rs->SwizzleString = swizzleStr.ToString();
                rs->Operand = base;
                codeWriter.Insert(rs);
                PushStack(rs);
                return expr;
            }
            virtual RefPtr<ExpressionSyntaxNode> VisitMemberExpression(MemberExpressionSyntaxNode * expr) override
            {
                RefPtr<Object> refObj;
                expr->BaseExpression->Access = expr->Access;
                expr->BaseExpression->Accept(this);
                auto base = PopStack();
                if (auto declRefType = expr->BaseExpression->Type->AsDeclRefType())
                {
                    if (auto structDecl = declRefType->declRef.As<StructDeclRef>())
                    {
                        int id = structDecl.GetDecl()->FindFieldIndex(expr->MemberName);
                        GenerateIndexExpression(base, program->ConstantPool->CreateConstant(id));
                    }
                    else
                        throw NotImplementedException("member expression codegen");
                }
                else
                    throw NotImplementedException("member expression codegen");
               
                return expr;
            }
            virtual RefPtr<ExpressionSyntaxNode> VisitInvokeExpression(InvokeExpressionSyntaxNode* expr) override
            {
                List<ILOperand*> args;
                for (auto arg : expr->Arguments)
                {
                    arg->Accept(this);
                    args.Add(PopStack());
                }
                if (auto funcType = expr->FunctionExpr->Type.Ptr()->As<FuncType>())
                {
					CallInstruction * instr = nullptr;
					// ad-hoc processing for ctor calls
					if (auto ctor = dynamic_cast<ConstructorDecl*>(funcType->declRef.GetDecl()))
					{
						RefPtr<ExpressionType> exprType = DeclRefType::Create(DeclRef(ctor->ParentDecl, funcType->declRef.substitutions));
						auto rsType = TranslateExpressionType(exprType);
						instr = new CallInstruction(args.Count());
						instr->Type = rsType;
						instr->Function = "__init";
					}
					else
					{
						if (auto memberFunc = expr->FunctionExpr.As<MemberExpressionSyntaxNode>())
						{
							memberFunc->BaseExpression->Accept(this);
							auto thisPtr = PopStack();
							args.Insert(0, thisPtr);
						}
						else if (auto varFunc = expr->FunctionExpr.As<VarExpressionSyntaxNode>())
						{
							// check if function name is an implcit member, and add this argument if necessary
							if (auto stype = dynamic_cast<AggTypeDecl*>(varFunc->declRef.decl->ParentDecl))
							{
								if (thisArg)
									args.Insert(0, thisArg);
							}
						}
						instr = new CallInstruction(args.Count());
						ILFunction * func = nullptr;
						if (functions.TryGetValue(funcType->declRef, func))
						{
							// this is a user-defined function, set instr->Function as internal function name
							auto rsType = funcType->declRef.GetResultType();
							instr->Type = TranslateExpressionType(rsType);
							instr->Function = func->Name;
						}
						else
						{
							// this is an intrinsic function, set instr->Function as original function name
							instr->Type = TranslateExpressionType(expr->Type);
							if (auto member = expr->FunctionExpr.As<MemberExpressionSyntaxNode>())
								instr->Function = member->MemberName;
							else
								instr->Function = expr->FunctionExpr.As<VarExpressionSyntaxNode>()->Variable;
						}
					}
                    for (int i = 0; i < args.Count(); i++)
                        instr->Arguments[i] = args[i];
                    instr->Type = TranslateExpressionType(expr->Type);
                    codeWriter.Insert(instr);
                    PushStack(instr);
                }
                else
                    throw InvalidProgramException();
                return expr;
            }
            virtual RefPtr<ExpressionSyntaxNode> VisitOperatorExpression(OperatorExpressionSyntaxNode* expr) override
            {
                if (expr->Operator == Operator::PostDec || expr->Operator == Operator::PostInc
                    || expr->Operator == Operator::PreDec || expr->Operator == Operator::PreInc)
                {
                    expr->Arguments[0]->Access = ExpressionAccess::Read;
                    expr->Arguments[0]->Accept(this);
                    auto base = PopStack();
                    BinaryInstruction * instr;
                    if (expr->Operator == Operator::PostDec)
                        instr = new SubInstruction();
                    else
                        instr = new AddInstruction();
                    instr->Operands.SetSize(2);
                    instr->Operands[0] = base;
                    if (expr->Type->Equals(ExpressionType::Float))
                        instr->Operands[1] = program->ConstantPool->CreateConstant(1.0f);
                    else
                        instr->Operands[1] = program->ConstantPool->CreateConstant(1);
                    instr->Type = TranslateExpressionType(expr->Type);
                    codeWriter.Insert(instr);

                    expr->Arguments[0]->Access = ExpressionAccess::Write;
                    expr->Arguments[0]->Accept(this);
                    auto dest = PopStack();
                    auto store = new StoreInstruction(dest, instr);
                    codeWriter.Insert(store);
                    PushStack(base);
                }
                else if (expr->Operator == Operator::PreDec || expr->Operator == Operator::PreInc)
                {
                    expr->Arguments[0]->Access = ExpressionAccess::Read;
                    expr->Arguments[0]->Accept(this);
                    auto base = PopStack();
                    BinaryInstruction * instr;
                    if (expr->Operator == Operator::PostDec)
                        instr = new SubInstruction();
                    else
                        instr = new AddInstruction();
                    instr->Operands.SetSize(2);
                    instr->Operands[0] = base;
                    if (expr->Type->Equals(ExpressionType::Float))
                        instr->Operands[1] = program->ConstantPool->CreateConstant(1.0f);
                    else
                        instr->Operands[1] = program->ConstantPool->CreateConstant(1);
                    instr->Type = TranslateExpressionType(expr->Type);
                    codeWriter.Insert(instr);

                    expr->Arguments[0]->Access = ExpressionAccess::Write;
                    expr->Arguments[0]->Accept(this);
                    auto dest = PopStack();
                    auto store = new StoreInstruction(dest, instr);
                    codeWriter.Insert(store);
                    PushStack(instr);
                }
                else if (expr->Arguments.Count() == 1)
                {
                    expr->Arguments[0]->Accept(this);
                    auto base = PopStack();
                    auto genUnaryInstr = [&](ILOperand * input)
                    {
                        UnaryInstruction * rs = 0;
                        switch (expr->Operator)
                        {
                        case Operator::Not:
                            input = EnsureBoolType(input, expr->Arguments[0]->Type);
                            rs = new NotInstruction();
                            break;
                        case Operator::Neg:
                            rs = new NegInstruction();
                            break;
                        case Operator::BitNot:
                            rs = new BitNotInstruction();
                            break;
                        default:
                            throw NotImplementedException("Code gen is not implemented for this operator.");
                        }
                        rs->Operand = input;
                        rs->Type = input->Type;
                        codeWriter.Insert(rs);
                        return rs;
                    };
                    PushStack(genUnaryInstr(base));
                }
                else
                {
                    expr->Arguments[1]->Accept(this);
                    auto right = PopStack();
                    if (expr->Operator == Operator::Assign)
                    {
                        expr->Arguments[0]->Access = ExpressionAccess::Write;
                        expr->Arguments[0]->Accept(this);
                        auto left = PopStack();
                        Assign(left, right);
                        PushStack(left);
                    }
                    else
                    {
                        expr->Arguments[0]->Access = ExpressionAccess::Read;
                        expr->Arguments[0]->Accept(this);
                        auto left = PopStack();
                        BinaryInstruction * rs = 0;
                        switch (expr->Operator)
                        {
                        case Operator::Add:
                        case Operator::AddAssign:
                            rs = new AddInstruction();
                            break;
                        case Operator::Sub:
                        case Operator::SubAssign:
                            rs = new SubInstruction();
                            break;
                        case Operator::Mul:
                        case Operator::MulAssign:
                            rs = new MulInstruction();
                            break;
                        case Operator::Mod:
                        case Operator::ModAssign:
                            rs = new ModInstruction();
                            break;
                        case Operator::Div:
                        case Operator::DivAssign:
                            rs = new DivInstruction();
                            break;
                        case Operator::And:
                            rs = new AndInstruction();
                            break;
                        case Operator::Or:
                            rs = new OrInstruction();
                            break;
                        case Operator::BitAnd:
                        case Operator::AndAssign:
                            rs = new BitAndInstruction();
                            break;
                        case Operator::BitOr:
                        case Operator::OrAssign:
                            rs = new BitOrInstruction();
                            break;
                        case Operator::BitXor:
                        case Operator::XorAssign:
                            rs = new BitXorInstruction();
                            break;
                        case Operator::Lsh:
                        case Operator::LshAssign:
                            rs = new ShlInstruction();
                            break;
                        case Operator::Rsh:
                        case Operator::RshAssign:
                            rs = new ShrInstruction();
                            break;
                        case Operator::Eql:
                            rs = new CmpeqlInstruction();
                            break;
                        case Operator::Neq:
                            rs = new CmpneqInstruction();
                            break;
                        case Operator::Greater:
                            rs = new CmpgtInstruction();
                            break;
                        case Operator::Geq:
                            rs = new CmpgeInstruction();
                            break;
                        case Operator::Leq:
                            rs = new CmpleInstruction();
                            break;
                        case Operator::Less:
                            rs = new CmpltInstruction();
                            break;
                        default:
                            throw NotImplementedException("Code gen not implemented for this operator.");
                        }
                        rs->Operands.SetSize(2);
                        rs->Operands[0] = left;
                        rs->Operands[1] = right;
                        rs->Type = TranslateExpressionType(expr->Type);
                        codeWriter.Insert(rs);
                        switch (expr->Operator)
                        {
                        case Operator::AddAssign:
                        case Operator::SubAssign:
                        case Operator::MulAssign:
                        case Operator::DivAssign:
                        case Operator::ModAssign:
                        case Operator::LshAssign:
                        case Operator::RshAssign:
                        case Operator::AndAssign:
                        case Operator::OrAssign:
                        case Operator::XorAssign:
                        {
                            expr->Arguments[0]->Access = ExpressionAccess::Write;
                            expr->Arguments[0]->Accept(this);
                            auto target = PopStack();
                            Assign(target, rs);
                            break;
                        }
                        default:
                            break;
                        }
                        PushStack(rs);
                    }
                    return expr;
                }
                return expr;
            }
            bool GenerateVarRef(String name)
            {
                ILOperand * var = 0;
                String srcName = name;
                if (!variables.TryGetValue(srcName, var))
                {
                    if (thisDeclRef)
                    {
                        int id = ((AggTypeDecl*)thisDeclRef.GetDecl())->FindFieldIndex(name);
                        GenerateIndexExpression(thisArg, program->ConstantPool->CreateConstant(id));
                        return true;
                    }
                    return false;
                }
                PushStack(var);
                return true;
            }
            virtual RefPtr<ExpressionSyntaxNode> VisitVarExpression(VarExpressionSyntaxNode* expr) override
            {
                RefPtr<Object> refObj;
                if (!GenerateVarRef(expr->Variable))
                {
                    throw InvalidProgramException("identifier is neither a variable nor a recognized component.");
                }
                return expr;
            }
        };

        SyntaxVisitor * CreateILCodeGenerator(DiagnosticSink * err, ILProgram * program, CompileOptions * options)
        {
            return new ILGenerator(program, err, *options);
        }
    }
}