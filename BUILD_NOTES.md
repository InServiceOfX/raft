# Requirements

* `ninja`
  - `sudo apt-get install ninja-build`

# Build Notes

- on NVIDIA Geforce 980Ti
- on 698d6c7b2203ed423d283bc268796eb828b2098d

When running `./build.sh `

```
/home/propdev/Prop/raft/cpp/build/_deps/cccl-src/cub/cub/cmake/../../cub/agent/agent_histogram.cuh(374): error: no instance of overloaded function "atomicAdd" matches the argument list
            argument types are: (double *, double)
                          atomicAdd(privatized_histograms[CHANNEL] + bins[PIXEL], accumulator);
                          ^
          detected during:
            instantiation of "void cub::CUB_200200_520_NS::AgentHistogram<AgentHistogramPolicyT, PRIVATIZED_SMEM_BINS, NUM_CHANNELS, NUM_ACTIVE_CHANNELS, SampleIteratorT, CounterT, PrivatizedDecodeOpT, OutputDecodeOpT, OffsetT, LEGACY_PTX_ARCH>::AccumulatePixels(

...

6 errors detected in the compilation of "/home/propdev/Prop/raft/cpp/src/raft_runtime/cluster/kmeans_fit_double.cu".
```

See [7.14.1.1. `atomicAdd()`](https://docs.nvidia.com/cuda/cuda-c-programming-guide/index.html#atomicadd), in particular signature `double atomicAdd(double* address, double val);` It says "The 64-bit floating-point version of atomicAdd() is only supported by devices of compute capability 6.x and higher." From ["Your GPU Compute Capability", CUDA-Enabled GeForce and TITAN Products](https://developer.nvidia.com/cuda-gpus), GeForce GTX 980 Ti has compute capability of **5.2**.

But `$ ./build.sh libraft --cache-tool=ccache` seems to work:

```
-- Installing: /home/propdev/Prop/raft/cpp/build/install/lib/cmake/raft/raft-dependencies.cmake
-- Installing: /home/propdev/Prop/raft/cpp/build/install/lib/cmake/raft/raft-config-version.cmake
```

Consider `raft/build.sh`. Either the "tests" target option, or `--compile-lib` or `--compile-static-lib` options would result in `COMPILE_LIBRARY=ON`. `RAFT_COMPILE_LIBRARY` variable in CMake will then get set by value of `COMPILE_LIBRARY` in the line for `cmake` around line 425.

Continue to trace that variable to `raft/cpp/CMakeLists.txt` and there it will compile, with the command `add_library(..)` and the `.cu` CUDA source files. It's by `src/raft_runtime/cluster/kmeans_fit_double.cu` that it fails on compiling on a 980 Ti for its compute capability of 5.2.

Instead, by commenting out the line for `src/raft_runtime/cluster/kmeans_fit_double.cu` in `raft/cpp/CMakeLists.txt` for `add_library(..)` command, we were able to build. Indeed, the build products are in `raft/cpp/build/install/lib` and installed in `$ORIGIN`. `$ORIGIN` for the "RPATH" (runtime library path) appears to be `raft/cpp/build`.