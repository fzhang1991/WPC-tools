${dr_data}/bin64/drrun -t drcachesim -simulator_type reuse_distance -ipc_name data -- ${run_command}   &> data.txt &
${dr_inst}/bin64/drrun -t drcachesim -simulator_type reuse_distance -ipc_name inst -- ${run_command}  &> inst.txt &
${dr_branch}/bin64/drrun -t drcachesim -simulator_type view -ipc_name branch -- ${run_command} &> branch.txt &
