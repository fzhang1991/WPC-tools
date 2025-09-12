py_path=../replace.py
h_path=../zfall.h

for p in conv list matrix
do
    for ol in 0 1 2 3 
    do
    c_path=../zfall_${p}.cpp
    log=log_gcc/${p}_O${ol}

    c_file="${c_path##*/}"
    file_name="${c_file%.*}"


    for n in 10000
    do
        for s in 16 32 64
        do
            s=`expr ${s} \* 1024`
            for t in INT_TYPE FP32_TYPE FP64_TYPE
            do
                for d in 1 2 3
                do
		    set -x
                    suffix=${s}_${t}_${d}
                    python3 ${py_path} -p ${h_path} -s ${s} -t ${t} -d ${d}
                    mkdir -p ${log}/${n}
                    o_file=${log}/${n}/${file_name}_${suffix}
                    g++ -static -O${ol} ${c_path} -o ${o_file}
		    #clang++ ${c_path} -g -O${ol} -std=c++1y -o ${o_file}
                    perf stat -r 3 -e 'L1-dcache-misses,instructions,L1-icache-misses,branch-misses,branch-load-misses,branch-loads,cycles' ./${o_file} -n ${n} &> ${o_file}.txt
                    #perf stat -r 3 -e ' r0283, r00C0, r00C5, L1-dcache-misses,instructions,branch-instructions' ./${o_file} -n ${n} &> ${o_file}.txt
		    set +x
                done
            done
        done
    done
    done

done

