#pragma once

#include "dl_base.hpp"
#include "dl_base_elemwise.hpp"

namespace dl {
namespace base {
/**
 * @brief int8 element-wise xor, support multidirectional broadcasting from 1D to 4D
 *
 * @param args elemwiseArgsType
 */
void elemwise_xor(elemwiseArgsType<int8_t> *args);

/**
 * @brief int16 element-wise xor, support multidirectional broadcasting from 1D to 4D
 *
 * @param args elemwiseArgsType
 */
void elemwise_xor(elemwiseArgsType<int16_t> *args);

} // namespace base
} // namespace dl
