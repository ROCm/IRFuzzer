# Fuzzing isel using AFL++

# Quick start

## Compile 

You should be able to prepare everything by running `./build.sh`. It should compile everything for you.

The script will set some environment variables. You may want to leave these in your `.bashrc` for further fuzzing:

```sh
# Path to this directory
export FUZZING_HOME=$(pwd)
# The LLVM you want to fuzz
export LLVM=llvm-aie
export AFL=AFLplusplus
export PATH=$PATH:$HOME/clang+llvm/bin
# Tell AFL++ to only use our mutator
export AFL_CUSTOM_MUTATOR_ONLY=1
# Tell AFL++ Where our mutator is
export AFL_CUSTOM_MUTATOR_LIBRARY=$FUZZING_HOME/mutator/build/libAFLCustomIRMutator.so
```

## Run

### Env vars

You can specify different arguments for the driver using environment variables.

**Required**

```
export TRIPLE=aie
```
You can specify other triples, e.g., `x86_64`, `aarch64`. 

```
export MATCHER_TABLE_SIZE=13780
```
Matcher table size refers to the size of the matcher table generated by TableGen. 
The table is automatically generated as a static variable in  in `SelectCode(SDNode *N) <Target>GenDAGISel.inc`(For SelectionDAG) and in `<Target>InstructionSelector::getMatchTable() <Target>GenGlobalISel.inc`(For GlobalIsel). You have three ways to find its length:

1. every time AFL's compiler compiles the project, it counts the table size and pops a `[+] MatcherTable size: 22660`. You can look out for that.
2. If you missed it, you can delete the object file (`ISelDAGToDAG.cpp.o` or `InstructionSelector.cpp.o`) and force a re-compilation.
```
$ cd build-afl
$ rm lib/Target/AIE/CMakeFiles/LLVMAIECodeGen.dir/AIEISelDAGToDAG.cpp.o
$ ninja

[6/27] Building CXX object lib/Target/AIE/CMakeFiles/LLVMAIECodeGen.dir/AIEISelDAGToDAG.cpp.o
[+] MatcherTable size: 22660
```
3. You can also find this data in `build.sh:120`. It may not be 100% accurate as the code gets updated. 

 

**Optional**

```
export GLOBAL_ISEL=1;
```
By default, we are fuzzing SelectionDAG. If you want to fuzz GlobalIsel, attach this environment variable. Please make sure `MATCHER_TABLE_SIZE` matches with GlobalIsel's table size.

### Command line

The easiest way to start fuzzing is to do
```
export FUZZING_INSTANCES=2
./fuzz.sh $FUZZING_INSTANCES
```
It would start arbitrary independent fuzzing instants to fuzz both SelectionDAG and GlobalIsel.

If you want to go into details or start a more customized fuzzing, an example to start fuzzing will look like:
```
# Fuzzing SelectionDAG
TRIPLE=aie MATCHER_TABLE_SIZE=22600 $FUZZING_HOME/$AFL/afl-fuzz -i $FUZZING_HOME/seeds/ -o $FUZZING_HOME/fuzzing_output  -w $FUZZING_HOME/llvm-isel-afl/build/isel-fuzzing;

# Fuzzing GlobalIsel
GLOBAL_ISEL=1 TRIPLE=aie MATCHER_TABLE_SIZE=22600; $FUZZING_HOME/$AFL/afl-fuzz -i $FUZZING_HOME/seeds/ -o $FUZZING_HOME/fuzzing_output  -w $FUZZING_HOME/llvm-isel-afl/build/isel-fuzzing;
```

Fuzzing can take weeks, if not days. We recommend using [`screen`](https://www.gnu.org/software/screen/) to run the fuzzing in the background.

### Archs and table size

Check `./script/common.py`.

# How do we fuzz

TODO: This will eventually evolve into Sec 3. Design in our paper. So the description is not perfect yet. It's just a brain dump.

## Program monitoring

In traditional fuzzing scenarios, it is believed control flow changes can be used to monitor program behaviors.
However, instruction selection has table methods. 
In SelectionDAG and GlobalIsel, the selection process can be described as a loop with a switch inside.
Whenever an SDNode/MachineInstr needs to be selected, we would start from the head of the table.
The loop will take an "Opcode" out of the table in every iteration. Based on the "Opcode," the switch goes to different branches and may consume more entries in the table to decide if a particular pattern fits the current SDNode/MachineInstr.
If the pattern doesn't match, the loop continues to next pattern by taking another "Opcode" out. Otherwise, the loop quits.

In other words, the loop can be treated as a "virtual machine" that "executes" the matcher table.
Therefore, traditional branch coverage is inefficient here. 
All the edges in the loop can be quickly exhausted even though the matcher table isn't.

Therefore, we track MatcherTable. 
We record every element that is indexed in the MatcherTable. 
If a new element is indexed, we just matched a new pattern, and the seed can be considered a new input.

We create a `ShadowMap` that has the same size as MatcherTable. Everytime there is a `MatcherTable[idx]`, we update `ShadowMap`. Thus if the `MATCHER_TABLE_SIZE` is set incorrectly, there will be an OOB Write at `ShadowMap.`

## Mutation

LLVM already provides a mutation framework. However, the work was incomplete and discontinued.
In our early version, we added vector support so the mutator could generate vector operations.
We found that vector operations can be a disaster, even for mature architectures like x86 or aarch64.

Vector operations only support a certain length, see [mutator.cpp:36](./mutator/src/mutator.cpp).
You can change the length and types you want by changing `addVectorTypeGetters.`
We are only testing "common" vectors we would see in the backend. 
However, fuzzing is about generating "uncommon" cases, so you are welcome to try other random things.

We also introduced instructions like `fneg` where it wasn't supported. 
Instructions like casting and truncation haven't been included yet but will be implemented later.

# Use this framework to fuzz scheduling

Fuzzing scheduling is hard since we have to observe two things:

1. Did the compiler crash?
2. Is the scheduled result correct?

It is easy to observe 1 when hard to observe 2 since most generated code is meanless and won't execute.

However, we can mutate on codes that have known semantics.
You can use codes from [`peano_usage`](https://gitenterprise.xilinx.com/XRLabs/peano_usage) as initial seeds.
After fuzzing, select those seeds that don't crash the compiler, run them in the simulator and see if the result changes. Since we only "add" garbage code to the seed, it shouldn't.

TODO: Add a new scheduling mutator to this repo and include usage.

# Trophies & Findings

(I think I will attach more links to keep track of these later)

## AI Engine
- AIE1 GlobalIsel lacks floating point support
    - G_FCONSTANT [fixed.](https://gitenterprise.xilinx.com/XRLabs/llvm-aie/pull/194)
- AIE1 GlobalIsel lacks vector support.
- AIE1 SelectionDAG has bugs in the memory store.
- AIE1 SelectionDAG has truncation errors. [Fixed.](https://gitenterprise.xilinx.com/XRLabs/llvm-aie/pull/161/)
- AIE1 `vst.spil` generates two stores to the same address. [PoC.](https://gitenterprise.xilinx.com/XRLabs/peano_usage/pull/15) [Fixed.](https://gitenterprise.xilinx.com/XRLabs/llvm-aie/pull/203)

## Open sourced architecture

**LLVM**
- SelectionDAG may cause infinite recursion on AArch64 and AIE. [Issue sent.](https://github.com/llvm/llvm-project/issues/57251)
- IRTranslator sign extends index value for G_EXTRACT_VECTOR_ELT, translating `i1 true` into `i32 -1`. [Issue sent.](https://github.com/llvm/llvm-project/issues/57452)
- Infinite recursion in DAGCombiner. [Issume sent.](https://github.com/llvm/llvm-project/issues/57658) [Fixing.](https://reviews.llvm.org/D133602)

**RISCV64**
- [Storing a float vector of size 1 after float arithmetic and branching causes assertion error `Invalid ANY_EXTEND`!](https://github.com/llvm/llvm-project/issues/58025) (Fixed)
- [Cannot scavenge register without an emergency spill slot](https://github.com/llvm/llvm-project/issues/58027) (Reported)

**AArch64**
- Double free in AArch64 GlobalIsel. [Issue sent.](https://github.com/llvm/llvm-project/issues/57282)
- AArch64 SelectionDAG uses uninitialized array and have OOB Write given long `shuffelvector` mask. [Issue sent.](https://github.com/llvm/llvm-project/issues/57326) [Fixing.](https://reviews.llvm.org/D132634)
- [[AArch64/GlobalISel] `fcmp true` / `fcmp false` used in `and` / `or` branching condition causes crash `Unknown FP condition!`](https://github.com/llvm/llvm-project/issues/58050) (Reported)
- [[AArch64/GlobalIsel] unable to legalize vectorized binaryop(G_ADD, G_SUB, ...)](https://github.com/llvm/llvm-project/issues/58156) (Reported)
- [[AArch64/GlobalISel] Unable to Translate `ret` with v1i8 / v1i16](https://github.com/llvm/llvm-project/issues/58211) (Reported)
- [[AArch64/GlobalISel] Cannot select `G_ZEXT` / `G_SEXT` / `G_ANYEXT` with v2i16](https://github.com/llvm/llvm-project/issues/58274) (Reported)

**X86_64**
- X86_64 SelectionDAG assertion failure on shift. [Fixed.](https://github.com/llvm/llvm-project/issues/57283)

**NVPTX**
- SelectionDAG Cannot select dynamic_stackalloc. [Issue sent.](https://github.com/llvm/llvm-project/issues/57398)
- DAG->DAG Pattern Instruction Selection crashes on mul i1. [Issue sent.](https://github.com/llvm/llvm-project/issues/57404)
- DAG->DAG Pattern Instruction Selection crashes on setcc. [Issue sent.](https://github.com/llvm/llvm-project/issues/57405)
- [[NVPTX] Assertion `CastInst::castIsValid(opc, C, Ty) && "Invalid constantexpr cast!"` failed](https://github.com/llvm/llvm-project/issues/58305) (Reported)

**AMDGPU**
- GlobalIsel AMDGPUPreLegalizerCombiner double frees on release build, OOB on debug build. [Issue sent.](https://github.com/llvm/llvm-project/issues/57406)
- GlobalIsel GlobalIsel crashes when extractelement index is invalid. [Issue sent.](https://github.com/llvm/llvm-project/issues/57408) [Fixing.](https://reviews.llvm.org/D132938)
- [[R600] Allocating Large Number of i1's on Stack Crashes with Error "Register number out of range"](https://github.com/llvm/llvm-project/issues/58171) (Reported)
- [[AMDGPU] No registers from class available to allocate for R600 / Cannot select for AMDGCN](https://github.com/llvm/llvm-project/issues/58210) (Reported)
- [[AMDGPU] mul used with v1i8 / v1i16 causes crash during IR optimizations due to type mismatch](https://github.com/llvm/llvm-project/issues/58331) (Fixed - Pending Merge)


The following code crashes `amdgcn` backend but is not reported.
```
define i32 @f(ptr %0) {
BB:
  %R = load i32, ptr %0
  ret i32 %R
}
```

# FAQ

__Why build two versions of LLVM?__

One version is built by AFL's compiler, and another is built by LLVM14 and contains a new mutator we designed. 
AFL needs to inject some code to the AIE compiler to keep track of runtime info (Edge coverage, MatcherTable coverage, etc.)
Besides, the driver also depends on it.
The other version is the dependency for the mutator. You __can__ use AFL instrumented mutator, but it would slow down mutation speed and thus not recommended.

__Why fuzz a fork of AIE that is not up-to-date?__

Mainly because mutator also needs to understand the architecture we are fuzzing, although it only generates mid-end IR.
Therefore, until we merge mutator's code into AIE, all you can do is keep merging the code you want to test to mutator branch and compile everything.

__Are we fuzzing AIE2?__

Currently we are only fuzzing AIE1 since it is more complete than AIE2. 
But you can fuzz AIE2 if you want to. In principle fuzzing AIE1 is no different than AIE2. 
All you need to do is set `TRIPLE=aie2` and set `MATCHER_TABLE_SIZE` correctly.

__AIE compilation hangs__

It's an known issue that `Target/AIE/MCTargetDesc/AIEMCFormats.cpp` will take a long time (~10 minutes) to compile. A function in it `__cxx_global_var_init()` will cause the optimizer to run for a really long time. It is an interesting bug, but we haven't had time to fix it.

__What is a seed and what to use__

Seed is the initial file you give fuzzer to work on. 
Unfortunately, this is required for AFL. (libFuzzer can cold-start without seed).
In this repo, we included a minimal seed in `seeds/` so you can start fuzzing without really worrying about it.

However, academic research and industry practice have shown that a better seed can lead to better results. You may reach the same result faster or find behavior unseen before with different seeds.
So if you can manually craft some seeds to cover different codes you want to test, for example, if you want to focus on floating point, you can create seeds with floating point calculations in them.

To create a seed, you can write LLVM IR manually and convert it to bitcode using `llvm-as`. Or you can cast bitcode to IR using `llvm-dis` and change some of the instructions.

__Matcher table coverage is 0.0%__

Table coverage may be low but never 0.0% in any cases. Please make sure the matcher table is correctly instrumented.

1. Make sure your binary is linked against the library compiled by AFL.
2. Make sure AFL instrumented it. During compilation, there should be a line telling you `[+] Instrumenting matcher table.`

__What does the stats in AFL's UI mean?__

You may check [this](https://github.com/mirrorer/afl/blob/master/docs/status_screen.txt) page to help you understand the stats.

We introduced a new coverage, so `map density` shows two stats. The first one is edge coverage, which should reach 70~80% in a day or two, meaning that (almost) all control flow has been tested. 
The second stat is matcher table coverage. It shows how much the table has been referenced. The higher, the better.

__My fuzzer is running slow__

There are two reasons it could happen.
AFL has high file system interactions. Therefore, make sure your directory is not a nfs or any remotely mounted hard drive. If you want even faster speed, you can mount a tmpfs to do fuzzing in the memory.

Another reason is your seeds are taking a long time to execute. You may either choose smaller initial seeds or use shorter timeouts by adding `-t <timeout>` to AFL's arguments.

__Where are the crashes located?__

`$FUZZING_HOME/fuzzing_output/default/crashes`

__How to reproduce errors?__

One upside of fuzzing is it always gives you reproducible PoC. 
You can run `build-release/bin/llc <args> <crashing-input>`.

__What if `MatcherTable` is not set or set incorrectly?__

To pass compilation and AFL's self-testing, `MATCHER_TABLE_SIZE` is defaulted to a small amount. You would most like to see `Shadow table size: 32 too small. Did you set it properly?` that means it is not set.
If `MATCHER_TABLE_SIZE` is not set correctly, you will have false positives where the seed is stored in `crashes` (Indicating the fuzzer finds the seed crashing), but you can't reproduce it with `llc`. 
That means the runtime code we injected is crashing, not the LLVM itself. Most likely, it's because `MATCHER_TABLE_SIZE` is set too small, and an OOB Write happened.
