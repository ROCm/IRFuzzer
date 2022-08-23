# Fuzzing isel using AFL++

# Quick start

## Compile 

You should be able to prepare everything by running `./build.sh`. It should compile everything for you.

Necessary env var will be exported, but you may want to leave these in your `.bashrc` for further fuzzing:

```sh
# Path to this directory
export FUZZING_HOME=$(pwd)
# The LLVM you want to fuzz
export LLVM=llvm-aie
# The LLVM that contains our mutator change. Currently it is a fork, hopefully it will be merged
export MUTATOR_LLVM=llvm-project
# AFL
export AFL=AFLplusplus
# Tell AFL++ to only use our mutator
export AFL_CUSTOM_MUTATOR_ONLY=1
# Tell AFL++ Where our mutator is
export AFL_CUSTOM_MUTATOR_LIBRARY=$FUZZING_HOME/mutator/build/libAFLCustomIRMutator.so
```

## Run

### Env vars

You can specify different arguments for the driver using environment vairables.

**Required**

```
export TRIPLE=aie
```
You can specify other triples, e.g. `x86_64`, `aarch64`. 

```
export MATCHER_TABLE_SIZE=13780
```
Matcher table size refers to the size of the matcher table generated by TableGen. 

1. Everytime AFL's compiler compiles the project, it counts the table size and pops a `[+] MatcherTable size: 22660`, you can look out for that.
2. If you missed, you can delete the object file (`ISelDAGToDAG.cpp.o` or `InstructionSelector.cpp.o`) and force a re-compilation.
```
$ cd build-afl
$ rm lib/Target/AIE/CMakeFiles/LLVMAIECodeGen.dir/AIEISelDAGToDAG.cpp.o
$ ninja

[6/27] Building CXX object lib/Target/AIE/CMakeFiles/LLVMAIECodeGen.dir/AIEISelDAGToDAG.cpp.o
[+] MatcherTable size: 22660
```
3. You can also find this data in `build.sh:120`. It may not be 100% accrate as the code gets updates. 

The table is automatically generated as a static table in  in `SelectCode(SDNode *N) <Target>GenDAGISel.inc`(For SelectionDAG) and in `<Target>InstructionSelector::getMatchTable() <Target>GenGlobalISel.inc`(For GlobalIsel). 

**Optional**

```
export GLOBAL_ISEL=1;
```
By default we are fuzzing SelectionDAG, if you want to fuzz GlobalIsel, attach this environment variable. Please make sure `MATCHER_TABLE_SIZE` matches with GlobalIsel's table size.

### Command line

The easiest way to start fuzzing is to do
```
export FUZZING_INSTANCES=2
./fuzz.sh $FUZZING_INSTANCES
```
It would start arbitrary independent fuzzing instants to fuzz both SelectionDAG and GlobalIsel.

If you want to go into details or start a more customized fuzzing, an example to start fuzzing would look like:
```
# Fuzzing SelectionDAG
TRIPLE=aie MATCHER_TABLE_SIZE=22600 $FUZZING_HOME/$AFL/afl-fuzz -i $FUZZING_HOME/seeds/ -o $FUZZING_HOME/fuzzing_output  -w $FUZZING_HOME/llvm-isel-afl/build/isel-fuzzing;

# Fuzzing GlobalIsel
GLOBAL_ISEL=1 TRIPLE=aie MATCHER_TABLE_SIZE=22600; $FUZZING_HOME/$AFL/afl-fuzz -i $FUZZING_HOME/seeds/ -o $FUZZING_HOME/fuzzing_output  -w $FUZZING_HOME/llvm-isel-afl/build/isel-fuzzing;
```

Fuzzing can take weeks if not days. I would recommended to use [`screen`](https://www.gnu.org/software/screen/) to run the fuzzing in the background.

# How do we fuzz

TODO: This will eventually evolve into Sec 3. Design in our paper. So the description is not perfect yet, it's just a braindump.

## Program monitoring

Traditional fuzzing scenarios believe programs behaviors can be monitored by control flow changes.
However, instruction selection has table methods. 
In both SelectionDAG and GlobalIsel, the selection process can be described as a loop with a switch inside.
Whenever a SDNode/MLIR needs to be selected, we would start from the head of the table.
In every iteration, the loop will take an "Opcode" out of the table. Based on the "Opcode", the switch goes to different branches and may consume more table to decide if a certain pattern fits current SDNode/MLIR.
If the pattern doesn't match, the loop continue to next pattern by taking another "Opcode" out, otherwise, the loop quits.

In another words, the loop itself can be treated as a "virtual machine" that "executes" the matcher table.
Therefore, traditional branch coverage is inefficient here. 
All the edges in the loop can be quickly exhausted even though the matcher table isn't.

Therefore, we specifically track MatcherTable. 
We remember every element that is referenced in the MatcherTable. 
If a new element is referenced, that means we just matched a new pattern, and the seed can be considered as a new input.

We create a `ShadowMap` that has the same size as MatcherTable. Everytime there is a `MatcherTable[idx]`, we update `ShadowMap`. Thus if the `MATCHER_TABLE_SIZE` is set incorrectly, there will be an OOB Write at `ShadowMap`.

## Mutation

LLVM already provides a mutation framework. However, the work incomplete and discontinued.
In our early version, we added vector support so the mutator can generate vector operations.
From what we found, vector operations can be a disaster, even for mature architectures like x86 or aarch64.

Vector operations only support a certain length, see [mutator.cpp:36](./mutator/src/mutator.cpp).
You can change length and types you want by changing `addVectorTypeGetters`.
For now we are only testing "common" vectors we would see in backend. 
However, the point of fuzzing is to generate "uncommon" cases, so you are welcome to try other random things.

We also introduced instructions like `fneg` where it wasn't supported. 
Instructions like casting and truncation haven't been included yet but will be implemented later.

# Use this framework to fuzz scheduling

Fuzzing scheduling is hard since we have to observe two things:

1. Did compiler crash?
2. Is the scheduled result correct?

It is easy to observe 1, when hard to observe 2 since most generated code is meanless and won't execute.

However, we can mutate on codes that have known semantics.
You can use codes from [`peano_usage`](https://gitenterprise.xilinx.com/XRLabs/peano_usage) as initial seed. 
After fuzzing, select those seeds that don't crash the compiler, run them in the simulator and see if the result changes. Since we only "add" garbage code to the seed, it shouldn't.

TODO: Add new scheduling mutator to this repo and include usage.

# Trophies & Findings

(I think I will attach more links to keep track of these later)

- AIE1 GlobalIsel lacks floating point support
    - [G_FCONSTANT fixed.](https://gitenterprise.xilinx.com/XRLabs/llvm-aie/pull/194)
- AIE1 GlobalIsel lacks vector support.
- AIE1 SelectionDAG has bugs in memory store.
- AIE1 SelectionDAG has trunction errors. [Fixed.](https://gitenterprise.xilinx.com/XRLabs/llvm-aie/pull/161/)
- AIE1 `vst.spil` generates two stores to the same address. [PoC](https://gitenterprise.xilinx.com/XRLabs/peano_usage/pull/15) [Fixed](https://gitenterprise.xilinx.com/XRLabs/llvm-aie/pull/203)
- SelectionDAG may cause infinite recursion. [Issues sent to LLVM community](https://github.com/llvm/llvm-project/issues/57251)
- Double free in AArch64 GlobalIsel. [Issues sent to LLVM community](https://github.com/llvm/llvm-project/issues/57282)
- Assertion failure in X64 SelectionDag. [Issues sent to LLVM community](https://github.com/llvm/llvm-project/issues/57283)

# FAQ

__What is seed and what to use__

Seed is the initial file you give fuzzer to work on. 
Unfortunatelly, for AFL this is requried. (libFuzzer can cold-start without seed thought).
In this repo, we included a minimal seed in `seeds/` so you can start fuzzing without really worry about it.

However, both academia research and industry practise have show that better seed can lead to better result. You may reach the same result faster, or find behavior unseed before with different seeds.
So if you can manually craft some seeds to cover different codes you want to test. For example, if you want to focus on floating point, you can create seeds with floating point calculation in it.

To create a seed, you can write LLVM IR manually and convert it to bitcode using `llvm-as`. Or you can cast bitcode to IR using `llvm-dis` and change some of the instructions.

__Where is the crashes located?__

`$FUZZING_HOME/fuzzing_output/default/crashes`

__How to reproduce errors?__

One upside of fuzzing is it always gives you reproducable PoC. 
You can run `build-release/bin/llc <args> <crashing-input>`.

__What if `MatcherTable` is not set or set incorrectly ?__

To pass compilation and AFL's self-testing, `MATCHER_TABLE_SIZE` is default to a small amount. You would most like to see `Shadow table size: 32 too small. Did you set it properly?`, that means it is not set.
If `MATCHER_TABLE_SIZE` is not set correctly, you will have false positives where the seed is stored in `crashes` (Indicating the fuzzer finds the seed crashing), but you can't reproduce it with `llc`. 
That means the runtime code we injected is crashing, not the seed itself. Mostly likely it's because `MATCHER_TABLE_SIZE` is set too small an a OOB Write happened.

__Why build two versions of AIE?__

One version is built by AFL's compiler, and another is built by LLVM14. 
AFL needs to inject some code to AIE compiler to keep trace of runtime info (Edge coverage, MatcherTable coverage, etc.)
Therefore, AFL driver also dependents on it.
The other version is the dependency for the mutator. You __can__ use AFL instrumented mutator, but it would slow down mutation speed and thus not recommended.
