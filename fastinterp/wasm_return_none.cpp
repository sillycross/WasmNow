#define POCHIVM_INSIDE_FASTINTERP_TPL_CPP

#include "fastinterp_tpl_common.hpp"
#include "wasm_common_ops_helper.h"

namespace PochiVM
{

struct FIReturnNoneImpl
{
    template<bool dummy>
    static constexpr bool cond()
    {
        return !dummy;
    }

    template<bool dummy>
    static void f(uintptr_t /*stackframe*/) noexcept
    {
        return;
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
    RegisterBoilerplate<FIReturnNoneImpl>(FIAttribute::NoContinuation);
}
