#define POCHIVM_INSIDE_FASTINTERP_TPL_CPP

#include "fastinterp_tpl_common.hpp"
#include "fastinterp_tpl_conditional_jump_helper.hpp"
#include "wasm_common_ops_helper.h"

namespace PochiVM
{

// Outlined conditional branch
//
struct FICondBranchImpl
{
    template<FINumOpaqueIntegralParams numOIP,
             FINumOpaqueFloatingParams numOFP,
             bool isInRegister>
    static constexpr bool cond()
    {
        if (isInRegister)
        {
            if (!FIOpaqueParamsHelper::CanPush(numOIP)) { return false; }
        }
        else
        {
            if (!FIOpaqueParamsHelper::IsEmpty(numOIP)) { return false; }
        }
        if (FIOpaqueParamsHelper::CanPush(numOFP)) { return false; }
        return true;
    }

    template<FINumOpaqueIntegralParams numOIP,
             FINumOpaqueFloatingParams numOFP,
             bool isInRegister,
             typename... OpaqueParams>
    static void f(uintptr_t stackframe, OpaqueParams... opaqueParams, [[maybe_unused]] uint32_t qa1) noexcept
    {
        uint32_t cond;
        if constexpr(isInRegister)
        {
            cond = qa1;
        }
        else
        {
            using SMA = StackMachineAccessor<uint32_t /*inputType*/,
                                             1 /*numOnStack*/,
                                             void /*outputType*/>;
            cond = SMA::template GetInput<0>(stackframe);
        }
        FIConditionalJumpHelper::execute_0_1<FIConditionalJumpHelper::Mode::OptForSizeMode, OpaqueParams...>(cond != 0, stackframe, opaqueParams...);
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
    RegisterBoilerplate<FICondBranchImpl>(FIAttribute::OptSize);
}
