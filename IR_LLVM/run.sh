#!/bin/bash
py_path=../replace.py
c_path=../zfall.cpp
h_path=../zfall.h

c_file="${c_path##*/}"
file_name="${c_file%.*}"
llvm_path=/data/benchcpu/benchcpu/llvm_profiling

for n in 10000 
do
    for s in 128 
    do
        s=`expr ${s} \* 1024`
        for t in INT_TYPE FP32_TYPE FP64_TYPE
        do
            for d in 1 2 3
            do
                set -x
                suffix=${s}_${t}_${d}
                python3 ${py_path} -p ${h_path} -s ${s} -t ${t} -d ${d}
                mkdir -p log/${n}
                o_file=log/${n}/${file_name}_${suffix}

                clang++ ${c_path} -g -O3 -std=c++1y -o ${o_file}
                clang++ -S -emit-llvm ${c_path} -o ${o_file}.ll

                ins_ofile=${o_file}_ins    
                cd ${llvm_path}/instruction_reuse_distance && make && cd -
                ${llvm_path}/instruction_reuse_distance/inst_reuse_dist ${o_file}.ll ${ins_ofile}.bc
                clang++ ${ins_ofile}.bc ${llvm_path}/runtime.cpp -g -O3 -std=c++1y -o ${ins_ofile} -I `llvm-config --includedir` `llvm-config --ldflags --system-libs --libs` 
                ${ins_ofile} -n ${n} &> ${ins_ofile}.txt &

                data_ofile=${o_file}_data   
                cd ${llvm_path}/data_reuse_distance && make && cd -
                ${llvm_path}/data_reuse_distance/data_reuse_dist ${o_file}.ll ${data_ofile}.bc
                clang++ ${data_ofile}.bc ${llvm_path}/runtime.cpp -g -O3 -std=c++1y -o ${data_ofile} -I `llvm-config --includedir` `llvm-config --ldflags --system-libs --libs` 
                ${data_ofile} -n ${n} &> ${data_ofile}.txt &
                
                branch_ofile=${o_file}_branch
                cd ${llvm_path}/branch_profiling && make && cd -
                ${llvm_path}/branch_profiling/branch_profiling ${o_file}.ll ${branch_ofile}.bc
                clang++ ${branch_ofile}.bc ${llvm_path}/runtime.cpp -g -O3 -std=c++1y -o ${branch_ofile} -I `llvm-config --includedir` `llvm-config --ldflags --system-libs --libs` 
                ${branch_ofile} -n ${n} &> ${branch_ofile}.txt &
                set +x
            done
        done
	wait
    done
done

wait
grep -A1 'the mean of reuse dist is' log/*/*
grep -A1 'the expect of reuse dist is' log/*/*
grep 'weighted linear entropy' log/*/*
