/*
 * Copyright (c) 2016-present Samsung Electronics Co., Ltd
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "Escargot.h"
#include "FunctionObject.h"
#include "ExecutionContext.h"
#include "Context.h"
#include "parser/ScriptParser.h"
#include "interpreter/ByteCode.h"
#include "interpreter/ByteCodeGenerator.h"
#include "interpreter/ByteCodeInterpreter.h"
#include "runtime/Environment.h"
#include "runtime/EnvironmentRecord.h"
#include "runtime/ErrorObject.h"
#include "util/Util.h"

namespace Escargot {

size_t g_functionObjectTag;

void FunctionObject::initFunctionObject(ExecutionState& state)
{
    // If Strict is true, then
    // Let thrower be the [[ThrowTypeError]] function Object (13.2.3).
    // Call the [[DefineOwnProperty]] internal method of F with arguments "caller", PropertyDescriptor {[[Get]]: thrower, [[Set]]: thrower, [[Enumerable]]: false, [[Configurable]]: false}, and false.
    // Call the [[DefineOwnProperty]] internal method of F with arguments "arguments", PropertyDescriptor {[[Get]]: thrower, [[Set]]: thrower, [[Enumerable]]: false, [[Configurable]]: false}, and false.
    bool isStrict = m_codeBlock->isStrict() && !m_codeBlock->hasCallNativeFunctionCode();

    if (isConstructor()) {
        m_structure = state.context()->defaultStructureForFunctionObject();
        m_values[ESCARGOT_OBJECT_BUILTIN_PROPERTY_NUMBER + 0] = (Value(Object::createFunctionPrototypeObject(state, this)));
        m_values[ESCARGOT_OBJECT_BUILTIN_PROPERTY_NUMBER + 1] = (Value(m_codeBlock->functionName().string()));
        m_values[ESCARGOT_OBJECT_BUILTIN_PROPERTY_NUMBER + 2] = (Value(m_codeBlock->parametersInfomation().size()));
        if (isStrict) {
            m_structure = state.context()->defaultStructureForFunctionObjectInStrictMode();
            auto data = state.context()->globalObject()->throwerGetterSetterData();
            m_values[ESCARGOT_OBJECT_BUILTIN_PROPERTY_NUMBER + 3] = Value(data);
            m_values[ESCARGOT_OBJECT_BUILTIN_PROPERTY_NUMBER + 4] = Value(data);
        }
    } else {
        m_structure = state.context()->defaultStructureForNotConstructorFunctionObject();
        m_values[ESCARGOT_OBJECT_BUILTIN_PROPERTY_NUMBER + 0] = (Value(m_codeBlock->functionName().string()));
        m_values[ESCARGOT_OBJECT_BUILTIN_PROPERTY_NUMBER + 1] = (Value(m_codeBlock->parametersInfomation().size()));
        if (isStrict) {
            m_structure = state.context()->defaultStructureForNotConstructorFunctionObjectInStrictMode();
            auto data = state.context()->globalObject()->throwerGetterSetterData();
            m_values[ESCARGOT_OBJECT_BUILTIN_PROPERTY_NUMBER + 2] = Value(data);
            m_values[ESCARGOT_OBJECT_BUILTIN_PROPERTY_NUMBER + 3] = Value(data);
        }
    }
}

FunctionObject::FunctionObject(ExecutionState& state, CodeBlock* codeBlock, ForBuiltin)
    : Object(state, ESCARGOT_OBJECT_BUILTIN_PROPERTY_NUMBER + 2, false)
    , m_codeBlock(codeBlock)
    , m_outerEnvironment(nullptr)
{
    ASSERT(!isConstructor());
    initFunctionObject(state);
}

FunctionObject::FunctionObject(ExecutionState& state, NativeFunctionInfo info)
    : Object(state, info.m_isConsturctor ? (ESCARGOT_OBJECT_BUILTIN_PROPERTY_NUMBER + 3) : (ESCARGOT_OBJECT_BUILTIN_PROPERTY_NUMBER + 2), false)
    , m_codeBlock(new CodeBlock(state.context(), info))
    , m_outerEnvironment(nullptr)
{
    initFunctionObject(state);
    setPrototype(state, state.context()->globalObject()->functionPrototype());
}

FunctionObject::FunctionObject(ExecutionState& state, NativeFunctionInfo info, ForBuiltin)
    : Object(state, ESCARGOT_OBJECT_BUILTIN_PROPERTY_NUMBER + 3, false)
    , m_codeBlock(new CodeBlock(state.context(), info))
    , m_outerEnvironment(nullptr)
{
    ASSERT(isConstructor());
    initFunctionObject(state);
    setPrototype(state, state.context()->globalObject()->functionPrototype());
    m_structure = state.context()->defaultStructureForBuiltinFunctionObject();
}

FunctionObject::FunctionObject(ExecutionState& state, CodeBlock* codeBlock, LexicalEnvironment* outerEnv)
    : Object(state,
             (codeBlock->isConsturctor() ? (ESCARGOT_OBJECT_BUILTIN_PROPERTY_NUMBER + 3) : (ESCARGOT_OBJECT_BUILTIN_PROPERTY_NUMBER + 2)) + ((codeBlock->isStrict() && !codeBlock->hasCallNativeFunctionCode()) ? 2 : 0),
             false)
    , m_codeBlock(codeBlock)
    , m_outerEnvironment(outerEnv)
{
    initFunctionObject(state);
    setPrototype(state, state.context()->globalObject()->functionPrototype());
}

NEVER_INLINE void FunctionObject::generateBytecodeBlock(ExecutionState& state)
{
    Vector<CodeBlock*, gc_allocator<CodeBlock*>>& v = state.context()->compiledCodeBlocks();

    size_t currentCodeSizeTotal = 0;
    for (size_t i = 0; i < v.size(); i++) {
        currentCodeSizeTotal += v[i]->m_byteCodeBlock->m_code.size();
        currentCodeSizeTotal += (v[i]->m_byteCodeBlock->m_locData.size() * sizeof(std::pair<size_t, size_t>));
        currentCodeSizeTotal += v[i]->m_byteCodeBlock->m_literalData.size() * sizeof(size_t);
        currentCodeSizeTotal += v[i]->m_byteCodeBlock->m_objectStructuresInUse->size() * sizeof(size_t);
        currentCodeSizeTotal += v[i]->m_byteCodeBlock->m_getObjectCodePositions.size() * sizeof(size_t);
    }
    // ESCARGOT_LOG_INFO("codeSizeTotal %lfMB\n", (int)currentCodeSizeTotal / 1024.0 / 1024.0);

    const size_t codeSizeMax = 1024 * 1024 * 2;
    if (currentCodeSizeTotal > codeSizeMax) {
        std::vector<CodeBlock*, gc_allocator<CodeBlock*>> codeBlocksInCurrentStack;

        ExecutionContext* ec = state.executionContext();
        while (ec) {
            auto env = ec->lexicalEnvironment();
            if (env->record()->isDeclarativeEnvironmentRecord() && env->record()->asDeclarativeEnvironmentRecord()->isFunctionEnvironmentRecord()) {
                auto cblk = env->record()->asDeclarativeEnvironmentRecord()->asFunctionEnvironmentRecord()->functionObject()->codeBlock();
                if (cblk->script() && cblk->byteCodeBlock()) {
                    if (std::find(codeBlocksInCurrentStack.begin(), codeBlocksInCurrentStack.end(), cblk) == codeBlocksInCurrentStack.end()) {
                        codeBlocksInCurrentStack.push_back(cblk);
                    }
                }
            }
            ec = ec->parent();
        }

        for (size_t i = 0; i < v.size(); i++) {
            if (std::find(codeBlocksInCurrentStack.begin(), codeBlocksInCurrentStack.end(), v[i]) == codeBlocksInCurrentStack.end()) {
                v[i]->m_byteCodeBlock = nullptr;
            }
        }

        v.clear();
        v.resizeWithUninitializedValues(codeBlocksInCurrentStack.size());

        for (size_t i = 0; i < codeBlocksInCurrentStack.size(); i++) {
            v[i] = codeBlocksInCurrentStack[i];
            if (v[i]->m_byteCodeBlock) {
                v[i]->m_byteCodeBlock->m_locData.clear();
            }
        }
    }
    ASSERT(!m_codeBlock->hasCallNativeFunctionCode());

    volatile int sp;
    size_t currentStackBase = (size_t)&sp;
#ifdef STACK_GROWS_DOWN
    size_t stackRemainApprox = STACK_LIMIT_FROM_BASE - (state.stackBase() - currentStackBase);
#else
    size_t stackRemainApprox = STACK_LIMIT_FROM_BASE - (currentStackBase - state.stackBase());
#endif

    auto ret = state.context()->scriptParser().parseFunction(m_codeBlock, stackRemainApprox);
    Node* ast = ret.first;

    ByteCodeGenerator g;
    m_codeBlock->m_byteCodeBlock = g.generateByteCode(state.context(), m_codeBlock, ast, ret.second, false, false, false);
    delete ast;

    v.pushBack(m_codeBlock);
}

Value FunctionObject::call(ExecutionState& state, const Value& receiverSrc, const size_t& argc, Value* argv, bool isNewExpression)
{
    volatile int sp;
    size_t currentStackBase = (size_t)&sp;
#ifdef STACK_GROWS_DOWN
    if (UNLIKELY((state.stackBase() - currentStackBase) > STACK_LIMIT_FROM_BASE)) {
#else
    if (UNLIKELY((currentStackBase - state.stackBase()) > STACK_LIMIT_FROM_BASE)) {
#endif
        ErrorObject::throwBuiltinError(state, ErrorObject::RangeError, "Maximum call stack size exceeded");
    }

    Context* ctx = state.context();
    bool isStrict = m_codeBlock->isStrict();

    if (m_codeBlock->hasCallNativeFunctionCode()) {
        const CodeBlock::FunctionParametersInfoVector& info = m_codeBlock->parametersInfomation();
        CallNativeFunction* code = (CallNativeFunction*)&m_codeBlock->byteCodeBlock()->m_code[0];
        FunctionEnvironmentRecordSimple record(this);
        LexicalEnvironment env(&record, outerEnvironment());
        ExecutionContext ec(ctx, state.executionContext(), &env, isStrict);
        ExecutionState newState(&state, &ec);

        size_t len = info.size();
        if (argc < len) {
            Value* newArgv = (Value*)alloca(sizeof(Value) * len);
            for (size_t i = 0; i < argc; i++) {
                newArgv[i] = argv[i];
            }
            for (size_t i = argc; i < len; i++) {
                newArgv[i] = Value();
            }
            argv = newArgv;
        }

        Value receiver = receiverSrc;
        // prepare receiver
        if (UNLIKELY(!isStrict)) {
            if (receiver.isUndefinedOrNull()) {
                receiver = ctx->globalObject();
            } else {
                receiver = receiver.toObject(state);
            }
        }

        try {
            return code->m_fn(newState, receiver, argc, argv, isNewExpression);
        } catch (const Value& v) {
            ByteCodeInterpreter::processException(newState, v, &ec, SIZE_MAX);
        }
    }

    // prepare ByteCodeBlock if needed
    if (UNLIKELY(m_codeBlock->byteCodeBlock() == nullptr)) {
        generateBytecodeBlock(state);
    }

    ByteCodeBlock* blk = m_codeBlock->byteCodeBlock();

    size_t registerSize = blk->m_requiredRegisterFileSizeInValueSize;
    size_t stackStorageSize = m_codeBlock->identifierOnStackCount();
    size_t literalStorageSize = blk->m_numeralLiteralData.size();
    Value* literalStorageSrc = blk->m_numeralLiteralData.data();
    size_t parameterCopySize = std::min(argc, m_codeBlock->functionParameters().size());

    // prepare env, ec
    FunctionEnvironmentRecord* record;
    ExecutionContext* ec;

    if (LIKELY(m_codeBlock->canAllocateEnvironmentOnStack())) {
        // no capture, very simple case
        record = new (alloca(sizeof(FunctionEnvironmentRecordSimple))) FunctionEnvironmentRecordSimple(this);
        ec = new (alloca(sizeof(ExecutionContext))) ExecutionContext(ctx, state.executionContext(), new (alloca(sizeof(LexicalEnvironment))) LexicalEnvironment(record, outerEnvironment()), isStrict);
    } else {
        if (LIKELY(m_codeBlock->canUseIndexedVariableStorage())) {
            record = new FunctionEnvironmentRecordOnHeap(this, argc, argv);
        } else {
            record = new FunctionEnvironmentRecordNotIndexed(this, argc, argv);
        }
        ec = new ExecutionContext(ctx, state.executionContext(), new LexicalEnvironment(record, outerEnvironment()), isStrict);
    }

    ExecutionState newState(ctx, &state, ec);

    Value* registerFile = (Value*)alloca((registerSize + stackStorageSize + literalStorageSize) * sizeof(Value));
    Value* stackStorage = registerFile + registerSize;

    {
        Value* literalStorage = stackStorage + stackStorageSize;
        for (size_t i = 0; i < literalStorageSize; i++) {
            literalStorage[i] = literalStorageSrc[i];
        }
    }

    // prepare receiver
    if (!isStrict) {
        if (receiverSrc.isUndefinedOrNull()) {
            stackStorage[0] = ctx->globalObject();
        } else {
            stackStorage[0] = receiverSrc.toObject(state);
        }
    } else {
        stackStorage[0] = receiverSrc;
    }

    // binding function name
    stackStorage[1] = this;
    if (UNLIKELY(m_codeBlock->m_isFunctionNameSaveOnHeap)) {
        if (m_codeBlock->canUseIndexedVariableStorage()) {
            ASSERT(record->isFunctionEnvironmentRecordOnHeap());
            ((FunctionEnvironmentRecordOnHeap*)record)->FunctionEnvironmentRecordOnHeap::setHeapValueByIndex(0, this);
        } else {
            record->setMutableBinding(state, m_codeBlock->functionName(), this);
        }
    }

    // prepare parameters
    if (UNLIKELY(m_codeBlock->needsComplexParameterCopy())) {
        for (size_t i = 2; i < stackStorageSize; i++) {
            stackStorage[i] = Value();
        }
        if (!m_codeBlock->canUseIndexedVariableStorage()) {
            for (size_t i = 0; i < parameterCopySize; i++) {
                record->setMutableBinding(state, m_codeBlock->functionParameters()[i], argv[i]);
            }
        } else {
            Value* parameterStorageInStack = stackStorage + 2;
            const CodeBlock::FunctionParametersInfoVector& info = m_codeBlock->parametersInfomation();
            for (size_t i = 0; i < info.size(); i++) {
                Value val(Value::ForceUninitialized);
                // NOTE: consider the special case with duplicated parameter names (**test262: S10.2.1_A3)
                if (i < argc)
                    val = argv[i];
                else if (info[i].m_index >= (int32_t)argc)
                    continue;
                else
                    val = Value();
                if (info[i].m_isHeapAllocated) {
                    ASSERT(record->isFunctionEnvironmentRecordOnHeap());
                    ((FunctionEnvironmentRecordOnHeap*)record)->FunctionEnvironmentRecordOnHeap::setHeapValueByIndex(info[i].m_index, val);
                } else {
                    parameterStorageInStack[info[i].m_index] = val;
                }
            }
        }
    } else {
        Value* parameterStorageInStack = stackStorage + 2;
        for (size_t i = 0; i < parameterCopySize; i++) {
            parameterStorageInStack[i] = argv[i];
        }

        for (size_t i = parameterCopySize + 2; i < stackStorageSize; i++) {
            stackStorage[i] = Value();
        }
    }

    if (UNLIKELY(m_codeBlock->usesArgumentsObject())) {
        generateArgumentsObject(newState, record, stackStorage);
    }

    // run function
    size_t unused;
    const Value returnValue = ByteCodeInterpreter::interpret(newState, blk, 0, registerFile, &unused);
    if (UNLIKELY(blk->m_shouldClearStack))
        clearStack<512>();

    return returnValue;
}

void FunctionObject::generateArgumentsObject(ExecutionState& state, FunctionEnvironmentRecord* fnRecord, Value* stackStorage)
{
    AtomicString arguments = state.context()->staticStrings().arguments;
    if (fnRecord->isFunctionEnvironmentRecordNotIndexed()) {
        auto result = fnRecord->hasBinding(state, arguments);
        if (UNLIKELY(result.m_index == SIZE_MAX)) {
            fnRecord->createMutableBinding(state, arguments, false);
            result = fnRecord->hasBinding(state, arguments);
        }
        fnRecord->setMutableBindingByIndex(state, result.m_index, arguments, fnRecord->createArgumentsObject(state, state.executionContext()));
    } else {
        const CodeBlock::IdentifierInfoVector& v = fnRecord->functionObject()->codeBlock()->identifierInfos();
        for (size_t i = 0; i < v.size(); i++) {
            if (v[i].m_name == arguments) {
                ASSERT(v[i].m_needToAllocateOnStack);
                stackStorage[v[i].m_indexForIndexedStorage] = fnRecord->createArgumentsObject(state, state.executionContext());
                break;
            }
        }
    }
}
}
