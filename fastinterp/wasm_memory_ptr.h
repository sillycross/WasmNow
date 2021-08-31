#pragma once

#include "pochivm/common.h"

namespace PochiVM
{

#define GS_RELATIVE __attribute__((address_space(256)))

template<typename T>
using WasmMemPtr = T GS_RELATIVE *;

#undef GS_RELATIVE

}   // namespace PochiVM
