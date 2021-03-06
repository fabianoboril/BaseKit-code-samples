//==============================================================
// Copyright © 2019 Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#include <math.h>  //for ceil
#include <array>
#include <iostream>

#include <CL/sycl.hpp>

#include <tbb/blocked_range.h>
#include <tbb/flow_graph.h>
#include <tbb/global_control.h>
#include <tbb/parallel_for.h>

using namespace cl::sycl;

constexpr cl::sycl::access::mode sycl_read = cl::sycl::access::mode::read;
constexpr cl::sycl::access::mode sycl_write = cl::sycl::access::mode::write;
constexpr cl::sycl::access::mode sycl_read_write = cl::sycl::access::mode::read_write;


// exception handler
/*
The exception_list parameter is an iterable list of std::exception_ptr objects.
But those pointers are not always directly readable.
So, we rethrow the pointer, catch it,  and then we have the exception itself.
Note: depending upon the operation there may be several exceptions.
*/
auto exception_handler = [](exception_list exceptionList) {
  for (std::exception_ptr const& e : exceptionList) {
    try {
      std::rethrow_exception(e);
    } catch (exception const& e) {
      std::terminate();  // exit the process immediately.
    }
  }
};

#define VERBOSE

const float ratio = 0.5;  // CPU to GPU offload ratio
const float alpha = 0.5;  // coeff for triad calculation

const size_t array_size = 16;
std::array<float, array_size> a_array;  // input
std::array<float, array_size> b_array;  // input
std::array<float, array_size> c_array;  // output

void PrintArr(const char* text, const std::array<float, array_size>& array) {
  std::cout << text;
  for (const auto& s : array) std::cout << s << ' ';
  std::cout << "\n";
}

using async_node_type = tbb::flow::async_node<float, double>;
using gateway_type = async_node_type::gateway_type;

class AsyncActivity {
  tbb::task_arena a;

 public:
  AsyncActivity() { a = tbb::task_arena{1, 0}; }
  void run(float offload_ratio, gateway_type& gateway) {
    gateway.reserve_wait();
    a.enqueue([&, offload_ratio]() {
      // Execute the kernel over a portion of the array range
      size_t array_size_sycl = ceil(array_size * offload_ratio);
      std::cout << "start index for GPU = 0; end index for GPU = "
                << array_size_sycl << "\n";
      const float coeff = alpha;  // coeff is a local varaible

      // By including all the SYCL work in a {} block, we ensure
      // all SYCL tasks must complete before exiting the block
      {  // starting SYCL code
        range<1> n_items{array_size_sycl};
        buffer<cl_float, 1> a_buffer(a_array.data(), n_items);
        buffer<cl_float, 1> b_buffer(b_array.data(), n_items);
        buffer<cl_float, 1> c_buffer(c_array.data(), n_items);

        queue q;
        q.submit([&](handler& h) {
              auto a_accessor = a_buffer.get_access<sycl_read>(h);
              auto b_accessor = b_buffer.get_access<sycl_read>(h);
              auto c_accessor = c_buffer.get_access<sycl_write>(h);

              h.parallel_for( n_items, [=](id<1> index) {
                    c_accessor[index] = a_accessor[index] + b_accessor[index] * coeff;
                  });  // end of the kernel -- parallel for
            })
        .wait_and_throw();  // end of the commands for the SYCL queue
      }  // end of the scope for SYCL code; wait unti queued work completes

      double sycl_result = 1.0;  // passing some numerical result/flag
      gateway.try_put(sycl_result);
      gateway.release_wait();
    });  // a.enqueue
  }      // run
};

int main() {
  // init input arrays
  for (int i = 0; i < array_size; i++) {
    a_array[i] = i;
    b_array[i] = i;
  }

  int nth = 4;  // number of threads
                // tbb::task_scheduler_init init { nth };

  auto mp = tbb::global_control::max_allowed_parallelism;
  tbb::global_control gc(mp, nth + 1);  // One more thread, but sleeping
  tbb::flow::graph g;

  // Source node:
  bool n = false;
  tbb::flow::source_node<float> in_node{g,
                                        [&](float& offload_ratio) {
                                          if (n) return false;
                                          offload_ratio = ratio;
                                          n = true;
                                          return true;
                                        },
                                        false};

  // CPU node
  tbb::flow::function_node<float, double> cpu_node{
      g, tbb::flow::unlimited, [&](float offload_ratio) -> double {
        size_t i_start = static_cast<size_t>(ceil(array_size * offload_ratio));
        size_t i_end = static_cast<size_t>(array_size);
        std::cout << "start index for CPU = " << i_start
                  << "; end index for CPU = " << i_end << "\n";

        tbb::parallel_for(tbb::blocked_range<size_t>{i_start, i_end},
                          [&](const tbb::blocked_range<size_t>& r) {
                            for (size_t i = r.begin(); i < r.end(); ++i)
                              c_array[i] = a_array[i] + alpha * b_array[i];
                          });
        double tbb_result = 1.0;  // passing some numerical result/flag
        return (tbb_result);
      }};

  // async node  -- GPU
  AsyncActivity async_act;
  async_node_type a_node{
      g, tbb::flow::unlimited,
      [&async_act](const float& offload_ratio, gateway_type& gateway) {
        async_act.run(offload_ratio, gateway);
      }};

  // join node
  using join_t =
      tbb::flow::join_node<std::tuple<double, double>, tbb::flow::queueing>;
  join_t node_join{g};

  // out node
  tbb::flow::function_node<join_t::output_type> out_node{
      g, tbb::flow::unlimited, [&](const join_t::output_type& times) {
        // Serial execution
        std::array<float, array_size> c_gold;
        for (size_t i = 0; i < array_size; ++i)
          c_gold[i] = a_array[i] + alpha * b_array[i];

        // Compare golden triad with heterogeneous triad
        if (!std::equal(std::begin(c_array), std::end(c_array),
                        std::begin(c_gold)))
          std::cout << "Heterogenous triad error.\n";
        else
          std::cout << "Heterogenous triad correct.\n";

#ifdef VERBOSE
        PrintArr("c_array: ", c_array);
        PrintArr("c_gold  : ", c_gold);
#endif
      }};  // end of out node

  // construct graph
  tbb::flow::make_edge(in_node, a_node);
  tbb::flow::make_edge(in_node, cpu_node);
  tbb::flow::make_edge(a_node, tbb::flow::input_port<0>(node_join));
  tbb::flow::make_edge(cpu_node, tbb::flow::input_port<1>(node_join));
  tbb::flow::make_edge(node_join, out_node);

  in_node.activate();
  g.wait_for_all();

  return 0;
}
