#include "pochivm/common.h"
#include "fastinterp_tpl_opaque_params.h"

namespace PochiVM
{

enum class NumIntegralParamsAfterBlock
{
    X_END_OF_ENUM = x_fastinterp_max_integral_params + 1
};

enum class NumFloatParamsAfterBlock
{
    X_END_OF_ENUM = x_fastinterp_max_floating_point_params + 1
};

}   // namespace PochiVM
