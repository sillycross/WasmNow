#define POCHIVM_INSIDE_FASTINTERP_TPL_CPP

#include "fastinterp_tpl_common.hpp"
#include "wasm_memory_ptr.h"
#include "wasm_common_ops_helper.h"

namespace PochiVM
{

struct FICallIndirectImpl
{
    template<FINumOpaqueIntegralParams numOIP,
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

    template<FINumOpaqueIntegralParams numOIP,
             FINumOpaqueFloatingParams numOFP,
             bool isInRegister,
             typename... OpaqueParams>
    static void f(uintptr_t stackframe, OpaqueParams... opaqueParams, [[maybe_unused]] uint32_t qa1) noexcept
    {
        using SMA = StackMachineAccessor<uint32_t /*inputType*/,
                                         1 - static_cast<int>(isInRegister) /*numOnStack*/,
                                         void /*outputType*/>;

        uint64_t operand;
        if constexpr(!isInRegister)
        {
            operand = SMA::template GetInput<0>(stackframe);
        }
        else
        {
            operand = qa1;
        }

        {
            DEFINE_INDEX_CONSTANT_PLACEHOLDER_3;
            if (operand >= CONSTANT_PLACEHOLDER_3)
            {
                DEFINE_BOILERPLATE_FNPTR_PLACEHOLDER_1(void(*)(uintptr_t, OpaqueParams...) noexcept);
                BOILERPLATE_FNPTR_PLACEHOLDER_1(stackframe, opaqueParams...);
                return;
            }
        }

        uint64_t ptr;
        {
            DEFINE_INDEX_CONSTANT_PLACEHOLDER_2;
            WasmMemPtr<uint64_t> p = reinterpret_cast<WasmMemPtr<uint64_t>>(operand * 16 + CONSTANT_PLACEHOLDER_2);
            uint64_t typ = p[0];
            DEFINE_INDEX_CONSTANT_PLACEHOLDER_4;
            if (typ != CONSTANT_PLACEHOLDER_4)
            {
                DEFINE_BOILERPLATE_FNPTR_PLACEHOLDER_1(void(*)(uintptr_t, OpaqueParams...) noexcept);
                BOILERPLATE_FNPTR_PLACEHOLDER_1(stackframe, opaqueParams...);
                return;
            }
            ptr = p[1];
        }

        *reinterpret_cast<uint64_t*>(stackframe) = ptr;

        DEFINE_BOILERPLATE_FNPTR_PLACEHOLDER_0(void(*)(uintptr_t, OpaqueParams...) noexcept);
        BOILERPLATE_FNPTR_PLACEHOLDER_0(stackframe, opaqueParams...);
    }

    static auto metavars()
    {
        return CreateMetaVarList(
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
    RegisterBoilerplate<FICallIndirectImpl>(FIAttribute::OptSize);
}
