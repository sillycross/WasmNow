# WasmNow: Fast WebAssembly Baseline Compiler

## IMPORTANT SECURITY NOTICE

This is a research project on JIT systems. Please, for your machine's security, **do NOT use this to run untrusted WebAssembly code!**

JIT systems are notoriously dangerous, since any bug in such systems has the potential of being weaponized into a full system takeover exploit. 
This is a project with research-grade code: bugs are inevitable, and none of the code have gone through any serious auditing, fuzzing and testing processes. I won't be surprised at all if an adversary can construct a malicious module that triggers a bug in my code, breaks out of the WebAssembly sandbox and gets shell access. 

If you want to run untrusted WebAssembly code, you should ALWAYS use the implementation from a major vendor like Chrome, Firefox or Safari. 

## Introduction

This is a prototype of a baseline compiler for WebAssembly. Unlike an optimizing compiler, the first priority of a baseline compiler is to compile code fast. Producing good code is still a goal, but a second priority. 

Our compiler employs a novel technique called copy-and-patch, which allows it to both compile significantly faster, and generate significantly better code compared with industrial state-of-the-art baseline compilers, including Chrome's Liftoff compiler and Wasmer's Singlepass compiler. 

Compared with Google Chrome's Liftoff compiler, on [Coremark benchmark](https://github.com/eembc/coremark), we compile 5x faster while also generating 39% faster code; on [PolyBenchC benchmark](https://github.com/MatthiasJReisinger/PolyBenchC-4.2.1/), we compile 6.5x faster while also generating 63% faster code.

More details of the copy-and-patch technique, the benchmarks, and another application of the technique on SQL query compilers can be found in paper [*Copy-and-Patch Compilation: A fast compilation algorithm for high-level languages and bytecode*](http://fredrikbk.com/publications/copy-and-patch.pdf), published on OOPSLA 2021 and received a Distinguished Paper Award.

## What is Implemented

We have support for WebAssembly 1.0 Core Specification. 

We have only partial support for WASI embedding, because I didn't have enough time to figure out the specification and implement full support. It currently runs Coremark and PolyBenchC benchmark with no issue, but probably you will hit error messages saying some WASI import is not implemented on other stuffs. I have shifted my focus to other research ideas, so I have no plan to fix this. However, any help would be appreciated.

The code is also (unfortunately) pretty crappy. This stuff is rushed out in like a month or two, and I have zero knowledge about WebAssembly before. And the codebase is ported from my other project, and large amounts of dead code is not deleted. So, if you want to read the code, I have warned you...

So, this project is only a proof of concept to demonstrate the potential of our copy-and-patch technique. It is far from industrial-ready or optimized.

## Building and Testing

This project is currently implemented as a Google Test unit. It only runs on Linux. You need to have docker installed. You can build it by running

```
python3 pochivm-build cmake production
python3 pochivm-build make production
```

(Yes, I haven't even changed the name from the other project...) 

Then you should see an executable named `main`. You can run 

```
taskset -c 1 ./main --gtest_list_tests
```
to see all benchmarks. The benchmark names should be self-explanatory.
Then you can run something like
```
taskset -c 1 ./main --gtest_filter=WasmCompilation.BenchmarkAll
```
to run individual benchmarks (the above command runs the `WasmCompilation.BenchmarkAll` benchmark for example). 
The `taskset` above binds the executable to a fixed CPU ID, which reduces noise caused by task scheduler switching the process between CPUs.

## License

The ultimate goal of this project is to make the world a better place. While a restrictive license may reduce certain misuses that contradict our values, we believe software license is not the best solution to such problems. 

Therefore, this project uses MIT license, a very permissive license. However, we humbly request you to consider refrain from using the software for projects that cause harm to the world. For more information and examples, see our [open-source software for a better world statement](oss-for-a-better-world.md).


