#define POCHIVM_INSIDE_FASTINTERP_TPL_CPP

#include "fastinterp_tpl_common.hpp"
#include "wasm_store_block_simple_result.h"
#include "wasm_common_ops_helper.h"

namespace PochiVM
{

struct FICallSwitchSfImpl
{
    template<bool dummy>
    static constexpr bool cond()
    {
        return !dummy;
    }

    template<bool dummy>
    static void f(uintptr_t /*stackframe*/, uint64_t /*u1*/, uint64_t /*u2*/, uint64_t /*u3*/, uintptr_t newStackFrame) noexcept
    {
        DEFINE_BOILERPLATE_FNPTR_PLACEHOLDER_0(void(*)(uintptr_t) noexcept);
        BOILERPLATE_FNPTR_PLACEHOLDER_0(newStackFrame);
    }

    static auto metavars()
    {
        return CreateMetaVarList(
                    CreateBoolMetaVar("dummy")
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
    RegisterBoilerplate<FICallSwitchSfImpl>();
}
