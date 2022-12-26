import logging
import common
import subprocess
import argparse
import os
import multiprocessing


def fuzz(argv):
    argv.output = os.path.abspath(argv.output)
    if os.path.exists(argv.output):
        logging.info(f"{argv.output} already exists.")
        if argv.on_exist == "force":
            logging.info(f"on_exist set to {argv.on_exist}, will force remove")
            subprocess.run(["rm", "-rf", argv.output])
        elif argv.on_exist == "abort":
            logging.info(f"on_exist set to {argv.on_exist}, won't work on it.")
            return

    if argv.isel == "gisel":
        global_isel = 1
        matcher_table_size = common.MATCHER_TABLE_SIZE_GISEL
    elif argv.isel == "dagisel":
        global_isel = 0
        matcher_table_size = common.MATCHER_TABLE_SIZE_DAGISEL
    else:
        logging.fatal("UNREACHABLE, isel not set.")

    cpu_attr_arch_list = []
    if argv.tier == 0:
        cpu_attr_arch_list = [
            ("", "", triple) for triple in common.TRIPLE_ARCH_MAP.keys()
        ]
    elif argv.tier == 1:
        cpu_attr_arch_list = common.CPU_ATTR_ARCH_LIST_TIER_1
    elif argv.tier == 2:
        cpu_attr_arch_list = common.CPU_ATTR_ARCH_LIST_TIER_2
    elif argv.tier == 3:
        cpu_attr_arch_list = common.CPU_ATTR_ARCH_LIST_TIER_3
    elif argv.set is not None:
        cpu_attr_arch_list = [tuple(s.split(" ")) for s in argv.set]
        # TODO: Also do some sanity check for the set we are given.

    else:
        logging.fatal("UNREACHABLE, both tier and set is not specified.")

    isel = "dagisel" if global_isel == 0 else "gisel"
    tuples = []
    for r in range(argv.repeat):
        for (cpu, attr, triple) in cpu_attr_arch_list:
            arch = common.TRIPLE_ARCH_MAP[triple]
            if arch not in matcher_table_size:
                logging.info(
                    f"Can't find {triple}({arch})s' matcher table size, not fuzzing "
                )
                continue
            tuples.append((r + argv.offset, cpu, attr, triple, arch))

    def process_creator(t):
        r, cpu, attr, triple, arch = t
        logging.info(f"Fuzzing {cpu} {attr} {triple} {arch} -- {r}.")
        target = triple
        if cpu != "":
            target += "-" + cpu
        if attr != "":
            target += "-" + attr
        name = f"{r}"
        verbose_name = f"{argv.fuzzer}-{isel}-{target}-{r}"
        proj_dir = f"{argv.output}/{argv.fuzzer}/{isel}/{target}/{name}"

        fuzz_cmd = (
            f"$FUZZING_HOME/$AFL/afl-fuzz -V {argv.time} -i {argv.input} -o $OUTPUT"
        )

        if argv.fuzzer == "aflplusplus":
            dockerimage = "aflplusplus"
            fuzzer_specific = f"""
            export AFL_CUSTOM_MUTATOR_ONLY=0
            export AFL_CUSTOM_MUTATOR_LIBRARY="";
            """
        elif argv.fuzzer == "libfuzzer":
            dockerimage = "libfuzzer"
            fuzzer_specific = f"""
            export AFL_CUSTOM_MUTATOR_ONLY=1
            export AFL_CUSTOM_MUTATOR_LIBRARY=$FUZZING_HOME/mutator/build/libAFLFuzzMutate.so;
            """
        elif argv.fuzzer == "irfuzzer":
            dockerimage = "irfuzzer"
            fuzzer_specific = f"""
            export AFL_CUSTOM_MUTATOR_ONLY=1
            export AFL_CUSTOM_MUTATOR_LIBRARY=$FUZZING_HOME/mutator/build/libAFLCustomIRMutator.so;
            """
            fuzz_cmd += " -w"
        else:
            logging.warn("UNREACHABLE")
        fuzz_cmd += " $FUZZING_HOME/llvm-isel-afl/build/isel-fuzzing"
        env_exporting = f"""
            {fuzzer_specific}
            export CPU={cpu};
            export ATTR={attr};
            export TRIPLE={triple};
            export GLOBAL_ISEL={global_isel};
            export MATCHER_TABLE_SIZE={matcher_table_size[arch]};
        """

        if argv.type == "screen":
            command = f"""
            {env_exporting}
            export OUTPUT={proj_dir}
            mkdir -p {proj_dir}
            screen -S {verbose_name} -dm bash -c "{fuzz_cmd}"

            sleep {argv.time+60}
            exit
            """.encode()
        elif argv.type == "docker":
            command = f"""
            export OUTPUT=$FUZZING_HOME/fuzzing
            mkdir -p {proj_dir}
            docker run --cpus=1 --name={verbose_name} --rm --mount type=tmpfs,tmpfs-size=1G,dst=$OUTPUT --env OUTPUT=$OUTPUT -v {proj_dir}:/output {dockerimage} bash -c "
                {env_exporting}
                {fuzz_cmd}
                mv $OUTPUT/default /output/default
            "
            # Keep track of this container before it quits.
            while [[ ! -z $(docker ps | grep {verbose_name}) ]]
            do
                sleep 1000
            done
            exit
            """.encode()
        elif argv.type == "stdout":
            command = f"""
            {env_exporting}
            export AFL_NO_UI=1
            export OUTPUT={proj_dir}
            mkdir -p {proj_dir}
            {fuzz_cmd}
            """.encode()
        else:
            logging.fatal("UNREACHABLE, type not set")
        process = subprocess.Popen(
            ["/bin/bash", "-c", command], stdout=subprocess.PIPE, stdin=subprocess.PIPE
        )
        # Sleep for 100ms so aflplusplus has time to bind core. Otherwise two fuzzers may bind to the same core.
        subprocess.run(["sleep", "1"])
        return process

    common.parallel_subprocess(tuples, argv.jobs, process_creator, None)


def main() -> None:
    parser = argparse.ArgumentParser(description="Run all fuzzers")
    parser.add_argument(
        "-i",
        "--input",
        type=str,
        default="./seeds/",
        help="The directory containing input seeds, default to ./seeds",
    )
    parser.add_argument(
        "-o",
        "--output",
        type=str,
        default="./fuzzing",
        help="The directory to store all organized ./fuzzing/",
    )
    parser.add_argument(
        "--on_exist",
        default="abort",
        choices=["abort", "force", "ignore"],
        help="Our action if the output directory already exists",
    )
    parser.add_argument(
        "--fuzzer",
        choices=["aflplusplus", "libfuzzer", "irfuzzer"],
        required=True,
        help="The fuzzer we are using for fuzzing.",
    )
    parser.add_argument(
        "-j",
        "--jobs",
        type=int,
        default=multiprocessing.cpu_count(),
        help="Max number of jobs parallel, default to all cores.",
    )
    parser.add_argument(
        "-r", "--repeat", type=int, default=3, help="Numbers to repeat one experiment."
    )
    parser.add_argument(
        "--isel",
        choices=["gisel", "dagisel"],
        required=True,
        help="The isel alorighm you want to run.",
    )
    parser.add_argument(
        "-t", "--time", type=str, default="5m", help="Total time to run fuzzers"
    )
    parser.add_argument(
        "--tier",
        type=int,
        choices=[0, 1, 2, 3],
        help="The set of triples to test. 0 corresponds to everything, 1 and 2 corresponds to Tier 1 and Tier 2, see common.py for more. Will be overriden by `--set`",
    )
    parser.add_argument(
        "--offset",
        type=int,
        help="The offset that we starts counting experiments.",
        default=0,
    )
    parser.add_argument("--set", nargs="+", type=str, help="Select the triples to run.")
    parser.add_argument(
        "--type",
        type=str,
        required=True,
        choices=["screen", "docker", "stdout"],
        help="The method to start fuzzing cluster.",
    )
    args = parser.parse_args()

    def convert_to_seconds(s: str) -> int:
        seconds_per_unit = {"s": 1, "m": 60, "h": 3600, "d": 86400, "w": 604800}
        return int(s[:-1]) * seconds_per_unit[s[-1]]

    args.time = convert_to_seconds(args.time)

    fuzz(args)


if __name__ == "__main__":
    logging.basicConfig()
    logging.getLogger().setLevel(logging.INFO)
    main()
