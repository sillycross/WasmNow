#define POCHIVM_INSIDE_FASTINTERP_TPL_CPP

#include "fastinterp_tpl_common.hpp"
#include "wasm_unary_ops.h"
#include "wasm_common_ops_helper.h"

namespace PochiVM
{

struct FIIntUnaryOpsImpl
{
    template<typename OperandType,
             WasmIntUnaryOps operatorType>
    static constexpr bool cond()
    {
        return std::is_same<OperandType, uint32_t>::value ||
               std::is_same<OperandType, uint64_t>::value;
    }

    template<typename OperandType,
             WasmIntUnaryOps operatorType,
             FINumOpaqueIntegralParams numOIP,
             FINumOpaqueFloatingParams numOFP,
             bool isInRegister>
    static constexpr bool cond()
    {
        if (FIOpaqueParamsHelper::CanPush(numOFP)) { return false; }
        if (!isInRegister)
        {
            if (!FIOpaqueParamsHelper::IsEmpty(numOIP)) { return false; }
        }
        else
        {
            if (!FIOpaqueParamsHelper::CanPush(numOIP, 1)) { return false; }
        }
        return true;
    }

    template<typename OperandType,
             WasmIntUnaryOps operatorType,
             FINumOpaqueIntegralParams numOIP,
             FINumOpaqueFloatingParams numOFP,
             bool isInRegister,
             bool spillOutput>
    static constexpr bool cond()
    {
        return true;
    }

    template<typename OperandType,
             WasmIntUnaryOps operatorType,
             FINumOpaqueIntegralParams numOIP,
             FINumOpaqueFloatingParams numOFP,
             bool isInRegister,
             bool spillOutput,
             typename... OpaqueParams>
    static void f(uintptr_t stackframe, OpaqueParams... opaqueParams, [[maybe_unused]] OperandType qa1) noexcept
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

        OperandType result;
        if constexpr(operatorType == WasmIntUnaryOps::Clz)
        {
            if constexpr(std::is_same<OperandType, uint32_t>::value)
            {
                result = (likely(operand != 0)) ? static_cast<uint32_t>(__builtin_clz(operand)) : 32;
            }
            else
            {
                result = (likely(operand != 0)) ? static_cast<uint64_t>(__builtin_clzl(operand)) : 64;
            }
        }
        else if constexpr(operatorType == WasmIntUnaryOps::Ctz)
        {
            if constexpr(std::is_same<OperandType, uint32_t>::value)
            {
                result = (likely(operand != 0)) ? static_cast<uint32_t>(__builtin_ctz(operand)) : 32;
            }
            else
            {
                result = (likely(operand != 0)) ? static_cast<uint64_t>(__builtin_ctzl(operand)) : 64;
            }
        }
        else
        {
            static_assert(operatorType == WasmIntUnaryOps::Popcnt);
            if constexpr(std::is_same<OperandType, uint32_t>::value)
            {
                result = static_cast<uint32_t>(__builtin_popcount(operand));
            }
            else
            {
                result = static_cast<uint64_t>(__builtin_popcountl(operand));
            }
        }

        if constexpr(!spillOutput)
        {
            DEFINE_BOILERPLATE_FNPTR_PLACEHOLDER_0(void(*)(uintptr_t, OpaqueParams..., OperandType) noexcept);
            BOILERPLATE_FNPTR_PLACEHOLDER_0(stackframe, opaqueParams..., result);
        }
        else
        {
            *SMA::GetOutputLoc(stackframe) = result;

            DEFINE_BOILERPLATE_FNPTR_PLACEHOLDER_0(void(*)(uintptr_t, OpaqueParams...) noexcept);
            BOILERPLATE_FNPTR_PLACEHOLDER_0(stackframe, opaqueParams...);
        }
    }

    static auto metavars()
    {
        return CreateMetaVarList(
                    CreateTypeMetaVar("operandType"),
                    CreateEnumMetaVar<WasmIntUnaryOps::X_END_OF_ENUM>("operatorType"),
                    CreateOpaqueIntegralParamsLimit(),
                    CreateOpaqueFloatParamsLimit(),
                    CreateBoolMetaVar("isInRegister"),
                    CreateBoolMetaVar("spillOutput")
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
    RegisterBoilerplate<FIIntUnaryOpsImpl>();
}
