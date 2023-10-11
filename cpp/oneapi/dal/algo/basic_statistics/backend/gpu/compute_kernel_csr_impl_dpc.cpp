/*******************************************************************************
* Copyright 2023 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#include "oneapi/dal/algo/basic_statistics/backend/gpu/compute_kernel.hpp"
#include "oneapi/dal/algo/basic_statistics/backend/gpu/compute_kernel_csr_impl.hpp"
#include "oneapi/dal/table/csr_accessor.hpp"
#include "oneapi/dal/backend/primitives/utils.hpp"
#include "oneapi/dal/util/common.hpp"
#include "oneapi/dal/detail/policy.hpp"
#include "oneapi/dal/backend/communicator.hpp"

#ifdef ONEDAL_DATA_PARALLEL

namespace oneapi::dal::basic_statistics::backend {

namespace de = dal::detail;
namespace bk = dal::backend;
namespace pr = dal::backend::primitives;

using method_t = method::sparse;
using task_t = task::compute;
using comm_t = bk::communicator<spmd::device_memory_access::usm>;
using input_t = compute_input<task_t, dal::csr_table>;
using result_t = compute_result<task_t>;
using descriptor_t = detail::descriptor_base<task_t>;

template <typename Float>
result_t compute_kernel_csr_impl<Float>::operator()(const bk::context_gpu& ctx, const descriptor_t& desc, const input_t& input) {
    auto queue = ctx.get_queue();
    const csr_table csr_tdata = input.get_data();
    const auto column_count = csr_tdata.get_column_count();
    const auto row_count = csr_tdata.get_row_count();
    auto result_options = desc.get_result_options();
    const auto nonzero_count = csr_tdata.get_non_zero_count();
    auto [csr_data, column_indices, row_offsets] = csr_accessor<const Float>(csr_tdata).pull(queue, { 0, -1 }, sparse_indexing::zero_based);
    auto csr_data_ptr = csr_data.get_data();
    auto column_indices_ptr = column_indices.get_data();

    using limits_t = std::numeric_limits<Float>;

    constexpr Float maximum = limits_t::max();

    auto result_data = pr::ndarray<Float, 2>::empty(queue, {res_opt_count_, column_count }, sycl::usm::alloc::device);
    auto result_data_ptr = result_data.get_mutable_data();

    const auto local_size = std::min(bk::device_max_wg_size(queue), bk::down_pow2(nonzero_count));
    const auto nd_range = bk::make_multiple_nd_range_2d({column_count, local_size}, {1, local_size});
    auto event = queue.submit([&](sycl::handler& cgh) {
        // Init local memory
        sycl::local_accessor<Float, 1> local_res_buf(local_size * res_opt_count_, cgh);
        sycl::local_accessor<std::uint32_t, 1> row_counter(local_size, cgh);

        cgh.parallel_for(nd_range, [=](auto item) {
            std::int32_t col_idx = item.get_global_id(0);
#if __SYCL_COMPILER_VERSION >= 20230828
            Float* work_group_buf =
                local_res_buf.template get_multi_ptr<sycl::access::decorated::yes>()
                .get_raw();
            std::uint32_t* row_counter_buf = 
                row_counter.template get_multi_ptr<sycl::access::decorated::yes>()
                .get_raw();
#else
            Float* work_group_buf = local_res_buf.get_pointer().get();
            std::uint32_t* row_counter_buf = row_counter.get_pointer().get();
#endif
            auto local_id = item.get_local_id(1);
            Float* local_min = work_group_buf + local_id * res_opt_count_ + 0;
            Float* local_max = work_group_buf + local_id * res_opt_count_ + 1;
            Float* local_sum = work_group_buf + local_id * res_opt_count_ + 2;
            Float* local_sum_squares = work_group_buf + local_id * res_opt_count_ + 3;
            Float* local_sum_squares_cent = work_group_buf + local_id * res_opt_count_ + 4;

            auto cur_row_counter = row_counter_buf + local_id;
            cur_row_counter[0] = 0;

            local_min[0] = maximum;
            local_max[0] = -maximum;
            local_sum[0] = Float(0);
            local_sum_squares[0] = Float(0);
            local_sum_squares_cent[0] = Float(0);

            for (std::int32_t data_idx = local_id; data_idx < nonzero_count; data_idx += local_size) {
                if (column_indices_ptr[data_idx] == col_idx) {
                    auto val = csr_data_ptr[data_idx];
                    local_min[0] = sycl::min<Float>(local_min[0], val);
                    local_max[0] = sycl::max<Float>(local_max[0], val);
                    local_sum[0] += val;
                    local_sum_squares[0] += val * val;
                    cur_row_counter[0] += 1;
                }
            }
            // Reduce local statistics using tree reduction
            for (std::uint32_t i = local_size / 2; i > 0; i /= 2) {
                item.barrier(sycl::access::fence_space::local_space);
                if (local_id < i) {
                    local_min[0] = sycl::min<Float>(local_min[0], work_group_buf[(local_id + i) * res_opt_count_ + 0]);
                    local_max[0] = sycl::max<Float>(local_max[0], work_group_buf[(local_id + i) * res_opt_count_ + 1]);
                    local_sum[0] += work_group_buf[(local_id + i) * res_opt_count_ + 2];
                    local_sum_squares[0] += work_group_buf[(local_id + i) * res_opt_count_ + 3];
                    cur_row_counter[0] += row_counter_buf[local_id + i];
                }
            }
            item.barrier(sycl::access::fence_space::local_space);
            if (local_id == 0) {
                // In case when some rows are zero it must to compare
                // min and max with zero
                local_min[0] = row_counter[0] == row_count ? local_min[0] : sycl::min<Float>(0, local_min[0]);
                local_max[0] = row_counter[0] == row_count ? local_max[0] : sycl::max<Float>(0, local_max[0]);
                // Copy computed data to global memory
                result_data_ptr[0 * column_count + col_idx] =  local_min[0];
                result_data_ptr[1 * column_count + col_idx] = local_max[0];
                result_data_ptr[2 * column_count + col_idx] = local_sum[0];
                result_data_ptr[3 * column_count + col_idx] = local_sum_squares[0];

                result_data_ptr[5 * column_count + col_idx] = local_sum[0] / row_count;
                result_data_ptr[6 * column_count + col_idx] = local_sum_squares[0] / row_count;
            }
            item.barrier(sycl::access::fence_space::local_space);
            auto mean_val = result_data_ptr[5 * column_count + col_idx];
            for (std::int32_t data_idx = local_id; data_idx < nonzero_count; data_idx += local_size) {
                if (column_indices_ptr[data_idx] == col_idx) {
                    auto val = csr_data_ptr[data_idx];
                    local_sum_squares_cent[0] += (val - mean_val) * (val - mean_val);
                }
            }
            // Reduce local sum squares centered using tree reduction
            for (std::uint32_t i = local_size / 2; i > 0; i /= 2) {
                item.barrier(sycl::access::fence_space::local_space);
                if (local_id < i) {
                    local_sum_squares_cent[0] += work_group_buf[(local_id + i) * res_opt_count_ + 4];
                }
            }
            if (local_id == 0) {
                result_data_ptr[4 * column_count + col_idx] = local_sum_squares_cent[0];
                result_data_ptr[7 * column_count + col_idx] = local_sum_squares_cent[0] / (row_count - 1);
                result_data_ptr[8 * column_count + col_idx] = sycl::sqrt(result_data_ptr[7 * column_count + col_idx]);
                result_data_ptr[9 * column_count + col_idx] = result_data_ptr[7 * column_count + col_idx] / mean_val;
            }
        });
    });
    event.wait_and_throw();
    return get_result(queue, result_data, result_options);
}

template class compute_kernel_csr_impl<float>;
template class compute_kernel_csr_impl<double>;

} // namespace oneapi::dal::basic_statistics::backend
#endif // ONEDAL_DATA_PARALLEL