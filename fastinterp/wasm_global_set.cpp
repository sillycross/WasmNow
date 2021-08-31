#define POCHIVM_INSIDE_FASTINTERP_TPL_CPP

#include "fastinterp_tpl_common.hpp"
#include "wasm_memory_ptr.h"
#include "wasm_common_ops_helper.h"

namespace PochiVM
{

struct FIGlobalSetImpl
{
    template<typename OperandType>
    static constexpr bool cond()
    {
        return std::is_same<OperandType, uint32_t>::value ||
               std::is_same<OperandType, uint64_t>::value ||
               std::is_same<OperandType, float>::value ||
               std::is_same<OperandType, double>::value;
    }

    template<typename OperandType,
             FINumOpaqueIntegralParams numOIP,
             FINumOpaqueFloatingParams numOFP,
             bool isInRegister>
    static constexpr bool cond()
    {
        if (std::is_floating_point<OperandType>::value)
        {
            if (FIOpaqueParamsHelper::CanPush(numOIP)) { return false; }
            if (!isInRegister)
            {
                if (!FIOpaqueParamsHelper::IsEmpty(numOFP)) { return false; }
            }
            else
            {
                if (!FIOpaqueParamsHelper::CanPush(numOFP, 1)) { return false; }
            }
        }
        else
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
        }
        return true;
    }

    template<typename OperandType,
             FINumOpaqueIntegralParams numOIP,
             FINumOpaqueFloatingParams numOFP,
             bool isInRegister,
             typename... OpaqueParams>
    static void f(uintptr_t stackframe, OpaqueParams... opaqueParams, [[maybe_unused]] OperandType qa1) noexcept
    {
        using SMA = StackMachineAccessor<OperandType /*inputType*/,
                                         1 - static_cast<int>(isInRegister) /*numOnStack*/,
                                         void /*outputType*/>;

        OperandType operand;
        if constexpr(!isInRegister)
        {
            operand = SMA::template GetInput<0>(stackframe);
        }
        else
        {
            operand = qa1;
        }

        WasmMemPtr<OperandType> ptr;
        {
            DEFINE_INDEX_CONSTANT_PLACEHOLDER_2;
            ptr = reinterpret_cast<WasmMemPtr<OperandType>>(-CONSTANT_PLACEHOLDER_2);
        }

        *ptr = operand;

        DEFINE_BOILERPLATE_FNPTR_PLACEHOLDER_0(void(*)(uintptr_t, OpaqueParams...) noexcept);
        BOILERPLATE_FNPTR_PLACEHOLDER_0(stackframe, opaqueParams...);
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
    RegisterBoilerplate<FIGlobalSetImpl>();
}
