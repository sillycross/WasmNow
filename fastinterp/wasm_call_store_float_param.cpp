#define POCHIVM_INSIDE_FASTINTERP_TPL_CPP

#include "fastinterp_tpl_common.hpp"
#include "wasm_store_block_simple_result.h"
#include "wasm_common_ops_helper.h"

namespace PochiVM
{

struct FICallStoreFloatParamImpl
{
    template<typename OperandType>
    static constexpr bool cond()
    {
        return std::is_same<OperandType, float>::value ||
               std::is_same<OperandType, double>::value;
    }

    template<typename OperandType,
             FINumOpaqueIntegralParams numOIP,
             FINumOpaqueFloatingParams numOFP,
             bool isInRegister>
    static constexpr bool cond()
    {
        if (FIOpaqueParamsHelper::CanPush(numOIP)) { return false; }
        if (!isInRegister && !FIOpaqueParamsHelper::IsEmpty(numOFP)) { return false; }
        return true;
    }

    template<typename OperandType,
             FINumOpaqueIntegralParams numOIP,
             FINumOpaqueFloatingParams numOFP,
             bool isInRegister,
             typename... OpaqueParams>
    static void f(uintptr_t stackframe, OpaqueParams... opaqueParams,
                  [[maybe_unused]] OperandType qa1,
                  uint8_t* newStackFrame) noexcept
    {
        using SMA = StackMachineAccessor<OperandType /*inputType*/,
                                         1 - static_cast<int>(isInRegister) /*numOnStack*/,
                                         OperandType /*outputType*/>;

        OperandType operand;
        if constexpr(!isInRegister)
        {
            operand = SMA::template GetInput<0>(stackframe);
        }
        else
        {
            operand = qa1;
        }

        {
            DEFINE_INDEX_CONSTANT_PLACEHOLDER_2;
            *reinterpret_cast<OperandType*>(newStackFrame + CONSTANT_PLACEHOLDER_2) = operand;
        }

        DEFINE_BOILERPLATE_FNPTR_PLACEHOLDER_0(void(*)(uintptr_t, OpaqueParams...,
                                                       uint8_t*) noexcept);
        BOILERPLATE_FNPTR_PLACEHOLDER_0(stackframe, opaqueParams..., newStackFrame);
    }

    static auto metavars()
    {
        return CreateMetaVarList(
                    CreateTypeMetaVar("operandType"),
                    CreateOpaqueIntegralParamsLimit(),
                    CreateOpaqueFloatParamsLimit(),
                    CreateBoolMetaVar("isInRegister")
        );
    }
};

}   // namespace PochiVM

// build_fast_interp_lib.cpp JIT entry point
//
extern "C"
void __pochivm_build_fast_interp_library__()
{
    using namespace PochiVM;
    RegisterBoilerplate<FICallStoreFloatParamImpl>();
}
