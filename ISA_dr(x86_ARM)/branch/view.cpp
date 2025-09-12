/* **********************************************************
 * Copyright (c) 2017-2023 Google, Inc.  All rights reserved.
 * **********************************************************/

/*
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of Google, Inc. nor the names of its contributors may be
 *   used to endorse or promote products derived from this software without
 *   specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL VMWARE, INC. OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

/* This trace analyzer requires access to the modules.log file and the
 * libraries and binary from the traced execution in order to obtain further
 * information about each instruction than was stored in the trace.
 * It does not support online use, only offline.
 */

#include "view.h"

#include <stdint.h>

#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "analysis_tool.h"
#include "dr_api.h"
#include "memref.h"
#include "memtrace_stream.h"
#include "raw2trace.h"
#include "raw2trace_directory.h"
#include "trace_entry.h"
#include "utils.h"

namespace dynamorio {
namespace drmemtrace {

const std::string view_t::TOOL_NAME = "View tool";
bool zf_cbr = false;
long zf_taken = 0;
long zf_untaken = 0;
long zf_last_addr = 0;
int zf_last_addr_size=0;
std::unordered_map<long, std::pair<long, long>> cbrm;
bool cbrm_inited=false;

analysis_tool_t *
view_tool_create(const std::string &module_file_path, uint64_t skip_refs,
                 uint64_t sim_refs, const std::string &syntax, unsigned int verbose,
                 const std::string &alt_module_dir)
{
    return new view_t(module_file_path, skip_refs, sim_refs, syntax, verbose,
                      alt_module_dir);
}
inline void updateCBRM(long ins_id, bool taken) {
    if (!cbrm_inited){
	cbrm.clear();
        cbrm_inited=true;
        std::cout << "[INFO: cbrm initialized" << "]\n";
    }	
    auto iter = cbrm.find(ins_id);
    if (iter == cbrm.end()) {
        cbrm[ins_id] = std::make_pair(taken, !taken);
    } else {
        iter->second.first += taken;
        iter->second.second += !taken;
    }
}

view_t::view_t(const std::string &module_file_path, uint64_t skip_refs, uint64_t sim_refs,
               const std::string &syntax, unsigned int verbose,
               const std::string &alt_module_dir)
    : module_file_path_(module_file_path)
    , knob_verbose_(verbose)
    , trace_version_(-1)
    , knob_skip_refs_(skip_refs)
    , skip_refs_left_(knob_skip_refs_)
    , knob_sim_refs_(sim_refs)
    , sim_refs_left_(knob_sim_refs_)
    , knob_syntax_(syntax)
    , knob_alt_module_dir_(alt_module_dir)
    , num_disasm_instrs_(0)
    , prev_tid_(-1)
    , filetype_(-1)
    , timestamp_(0)
    , has_modules_(true)
{
}

std::string
view_t::initialize_stream(memtrace_stream_t *serial_stream)
{
    serial_stream_ = serial_stream;
    print_header();
    dcontext_.dcontext = dr_standalone_init();
    if (module_file_path_.empty()) {
        has_modules_ = false;
    } else {
        std::string error = directory_.initialize_module_file(module_file_path_);
        if (!error.empty())
            has_modules_ = false;
    }
    if (!has_modules_) {
        // Continue but omit disassembly to support cases where binaries are
        // not available and OFFLINE_FILE_TYPE_ENCODINGS is not present.
        return "";
    }
    // Legacy trace support where binaries are needed.
    // We do not support non-module code for such traces.
    module_mapper_ =
        module_mapper_t::create(directory_.modfile_bytes_, nullptr, nullptr, nullptr,
                                nullptr, knob_verbose_, knob_alt_module_dir_);
    module_mapper_->get_loaded_modules();
    std::string error = module_mapper_->get_last_error();
    if (!error.empty())
        return "Failed to load binaries: " + error;
    dr_disasm_flags_t flags = IF_X86_ELSE(
        DR_DISASM_ATT,
        IF_AARCH64_ELSE(DR_DISASM_DR, IF_RISCV64_ELSE(DR_DISASM_RISCV, DR_DISASM_ARM)));
    if (knob_syntax_ == "intel") {
        flags = DR_DISASM_INTEL;
    } else if (knob_syntax_ == "dr") {
        flags = DR_DISASM_DR;
    } else if (knob_syntax_ == "arm") {
        flags = DR_DISASM_ARM;
    } else if (knob_syntax_ == "riscv") {
        flags = DR_DISASM_RISCV;
    }
    disassemble_set_syntax(flags);
    return "";
}

bool
view_t::parallel_shard_supported()
{
    return false;
}

void *
view_t::parallel_shard_init_stream(int shard_index, void *worker_data,
                                   memtrace_stream_t *shard_stream)
{
    return shard_stream;
}

bool
view_t::parallel_shard_exit(void *shard_data)
{
    return true;
}

std::string
view_t::parallel_shard_error(void *shard_data)
{
    // Our parallel operation ignores all but one thread, so we need just
    // the one global error string.
    return error_string_;
}

bool
view_t::should_skip(memtrace_stream_t *memstream, const memref_t &memref)
{
    if (skip_refs_left_ > 0) {
        skip_refs_left_--;
        // I considered printing the version and filetype even when skipped but
        // it adds more confusion from the memref counting than it removes.
        // A user can do two views, one without a skip, to see the headers.
        return true;
    }
    if (knob_sim_refs_ > 0) {
        if (sim_refs_left_ == 0)
            return true;
        sim_refs_left_--;
        if (sim_refs_left_ == 0 && timestamp_ > 0) {
            // Print this timestamp right before the final record.
            print_prefix(memstream, memref, timestamp_record_ord_);
            std::cerr << "<marker: timestamp " << timestamp_ << ">\n";
            timestamp_ = 0;
        }
    }
    return false;
}

bool
view_t::process_memref(const memref_t &memref)
{
    return parallel_shard_memref(serial_stream_, memref);
}
bool
view_t::parallel_shard_memref(void *shard_data, const memref_t &memref)
{
    memtrace_stream_t *memstream = reinterpret_cast<memtrace_stream_t *>(shard_data);
    // Even for -skip_refs we need to process the up-front version and type.
    if (memref.marker.type == TRACE_TYPE_MARKER) {
        switch (memref.marker.marker_type) {
        case TRACE_MARKER_TYPE_VERSION:
            // We delay printing until we know the tid.
            if (trace_version_ == -1) {
                trace_version_ = static_cast<int>(memref.marker.marker_value);
            } else if (trace_version_ != static_cast<int>(memref.marker.marker_value)) {
                error_string_ = std::string("Version mismatch across files");
                return false;
            }
            version_record_ord_ = memstream->get_record_ordinal();
            return true; // Do not count toward -sim_refs yet b/c we don't have tid.
        case TRACE_MARKER_TYPE_FILETYPE:
            // We delay printing until we know the tid.
            if (filetype_ == -1) {
                filetype_ = static_cast<intptr_t>(memref.marker.marker_value);
            } else if (filetype_ != static_cast<intptr_t>(memref.marker.marker_value)) {
                error_string_ = std::string("Filetype mismatch across files");
                return false;
            }
            filetype_record_ord_ = memstream->get_record_ordinal();
            if (TESTANY(OFFLINE_FILE_TYPE_ARCH_ALL, memref.marker.marker_value) &&
                !TESTANY(build_target_arch_type(), memref.marker.marker_value)) {
                error_string_ = std::string("Architecture mismatch: trace recorded on ") +
                    trace_arch_string(static_cast<offline_file_type_t>(
                        memref.marker.marker_value)) +
                    " but tool built for " + trace_arch_string(build_target_arch_type());
                return false;
            }
            return true; // Do not count toward -sim_refs yet b/c we don't have tid.
        case TRACE_MARKER_TYPE_TIMESTAMP:
            // Delay to see whether this is a new window.  We assume a timestamp
            // is always followed by another marker (cpu or window).
            // We can't easily reorder and place window markers before timestamps
            // since memref iterators use the timestamps to order buffer units.
            timestamp_ = memref.marker.marker_value;
            timestamp_record_ord_ = memstream->get_record_ordinal();
            if (should_skip(memstream, memref))
                timestamp_ = 0;
            return true;
        default: break;
        }
    }


    if (should_skip(memstream, memref))
        return true;

    //if (memref.instr.tid != 0) {
       // print_prefix(memstream, memref);
    //}

    if (memref.marker.type == TRACE_TYPE_MARKER) {
        //std::cerr << "TRACE_TYPE_MARKER"<<"\n";
        return true;
    }
    if (!type_is_instr(memref.instr.type) &&
        memref.data.type != TRACE_TYPE_INSTR_NO_FETCH) {
	//std::cerr << "no instr\n";
        return true;
    }

   // static constexpr int name_width = 12;

    //if (zf_cbr){
//	    print_prefix(memstream, memref);
  //  std::cerr << std::left << std::setw(name_width) << "ifetch" << std::right
    //          << std::setw(2) << memref.instr.size << " byte(s) @ 0x" << std::hex
      //        << std::setfill('0') << std::setw(sizeof(void *) * 2) << memref.instr.addr
        //      << std::dec << std::setfill(' ');
    //}

    if(zf_cbr){
    long zf_next_addr_p = zf_last_addr + zf_last_addr_size;
    long zf_next_addr_y = std::stol(std::to_string(memref.instr.addr));
    if(zf_next_addr_p == zf_next_addr_y){
	    updateCBRM(zf_last_addr,true);
	    zf_taken +=1;
            //std::cerr<<"taken: "<< zf_taken <<","<< zf_last_addr<<"," << zf_last_addr_size<<"," <<memref.instr.addr<<"\n";
    }else{
	    updateCBRM(zf_last_addr,false);
	    zf_untaken +=1;
	    //std::cerr<<"untaken: "<< zf_untaken<<"," << zf_last_addr<<"," << zf_last_addr_size<<"," <<memref.instr.addr<<"\n";
    }
    }
    if (!TESTANY(OFFLINE_FILE_TYPE_ENCODINGS, filetype_) && !has_modules_) {
        // We can't disassemble so we provide what info the trace itself contains.
        // XXX i#5486: We may want to store the taken target for conditional
        // branches; if added, we can print it here.
        // XXX: It may avoid initial confusion over the record-oriented output
        // to indicate whether an instruction accesses memory, but that requires
        // delayed printing.
        //std::cerr << " ";
        switch (memref.instr.type) {
        case TRACE_TYPE_INSTR: break;//std::cerr << "non-branch\n"; break;
        case TRACE_TYPE_INSTR_DIRECT_JUMP: break;// std::cerr << "jump\n"; break;
        case TRACE_TYPE_INSTR_INDIRECT_JUMP: break; //std::cerr << "indirect jump\n"; break;
        case TRACE_TYPE_INSTR_CONDITIONAL_JUMP:
          //  if (!zf_cbr){
	    //print_prefix(memstream, memref);
	    //std::cerr << std::left << std::setw(name_width) << "ifetch" << std::right
	//	      << std::setw(2) << memref.instr.size << " byte(s) @ 0x" << std::hex
	//	      << std::setfill('0') << std::setw(sizeof(void *) * 2) << memref.instr.addr
	//	      << std::dec << std::setfill(' ');
	    //}
	    //std::cerr << " conditional jump\n";
	    zf_last_addr = std::stol(std::to_string(memref.instr.addr));
	    zf_last_addr_size = memref.instr.size;
	    break;
        case TRACE_TYPE_INSTR_TAKEN_JUMP: break;//std::cerr << "taken conditional jump\n"; break;
        case TRACE_TYPE_INSTR_UNTAKEN_JUMP:
            //std::cerr << "untaken conditional jump\n";
            break;
        case TRACE_TYPE_INSTR_DIRECT_CALL: break;// std::cerr << "call\n"; break;
        case TRACE_TYPE_INSTR_INDIRECT_CALL: break;// std::cerr << "indirect call\n"; break;
        case TRACE_TYPE_INSTR_RETURN: break;//std::cerr << "return\n"; break;
        case TRACE_TYPE_INSTR_NO_FETCH: break;//std::cerr << "non-fetched instruction\n"; break;
        case TRACE_TYPE_INSTR_SYSENTER: break;//std::cerr << "sysenter\n"; break;
        default: error_string_ = "Unknown instruction type\n"; return false;
        }
	//if (zf_cbr){
	  //  std::cerr <<"\n";
	//}
	zf_cbr = (memref.instr.type == TRACE_TYPE_INSTR_CONDITIONAL_JUMP);
        ++num_disasm_instrs_;
        return true;
    }

    app_pc decode_pc;
    const app_pc orig_pc = (app_pc)memref.instr.addr;
    if (!TESTANY(OFFLINE_FILE_TYPE_ENCODINGS, filetype_)) {
        // Legacy trace support where we need the binaries.
        decode_pc = module_mapper_->find_mapped_trace_address(orig_pc);
        if (!module_mapper_->get_last_error().empty()) {
            error_string_ = "Failed to find mapped address for " +
                to_hex_string(memref.instr.addr) + ": " +
                module_mapper_->get_last_error();
            return false;
        }
    }

    ++num_disasm_instrs_;
    return true;
}

inline double linear_entropy(long taken, long untaken) {
    double p = (double) taken / (taken + untaken);
    double ret = 2.0 *std::min(p, 1-p);
    return ret;
}
bool
view_t::print_results()
{
    std::cerr << TOOL_NAME << " results:\n";
    std::cerr << std::setw(15) << num_disasm_instrs_ << " : total instructions\n";
    long zf_cbrn = zf_taken + zf_untaken;
    std::cerr << zf_cbrn<< ": total cbr instructions, "<< zf_taken<<" :taken\n";

    double weighted_linear_entropy = 0.0;
        for (auto& kv : cbrm) {
            auto& count = kv.second;
	    weighted_linear_entropy +=
                (count.first + count.second) * linear_entropy(count.first, count.second);
            // std::cout << "linear_entropy: " << linear_entropy(count.first, count.second) << std::endl;
            std::cout<<"kv:" <<kv.first<<": "<< count.first << ", " << count.second <<"\n";
        }
        weighted_linear_entropy /= zf_cbrn;
    std::cerr <<"branch linear entropy: "<<weighted_linear_entropy<<"\n";
    return true;
}

} // namespace drmemtrace
} // namespace dynamorio
