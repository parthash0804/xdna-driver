// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2024, Advanced Micro Devices, Inc. All rights reserved.

#ifndef _SHIMTEST_EXEC_BUF_H_
#define _SHIMTEST_EXEC_BUF_H_

#include "bo.h"

#include "experimental/xrt_elf.h"
#include "experimental/xrt_module.h"
#include "core/common/api/module_int.h"

#include "core/include/ert.h"
#include "core/common/cuidx_type.h"
#include <cstring>

class exec_buf {
public:
  exec_buf(bo& bo_execbuf, uint32_t op) :
    m_exec_buf_bo(bo_execbuf)
    , m_cmd_pkt(reinterpret_cast<ert_start_kernel_cmd *>(bo_execbuf.map()))
    , m_cmd_size(bo_execbuf.size())
    , m_op(op)
    , m_arg_cnt(0)
    , m_reg_idx(0)
  {
    std::memset(m_cmd_pkt, 0, m_cmd_size);
    m_cmd_pkt->state = ERT_CMD_STATE_NEW;
    m_cmd_pkt->opcode = m_op;
    m_cmd_pkt->type = ERT_CU;
    inc_pkt_count(sizeof(int32_t)); // One word for cu mask
  }

  static void
  set_cu_idx(bo& bo_execbuf, xrt_core::cuidx_type cu_idx)
  {
    auto cmd_pkt = reinterpret_cast<ert_start_kernel_cmd *>(bo_execbuf.map());
    cmd_pkt->cu_mask = 0x1 << cu_idx.index;
  }

  void
  set_cu_idx(xrt_core::cuidx_type cu_idx)
  {
    m_cmd_pkt->cu_mask = 0x1 << cu_idx.index;
  }

  void
  add_ctrl_bo(bo& bo_ctrl)
  {
    auto cmd_packet = reinterpret_cast<ert_start_kernel_cmd *>(m_exec_buf_bo.map());
    switch (m_op) {
    case ERT_START_CU:
      break;
    case ERT_START_NPU: {
      auto npu_data = get_ert_npu_data(cmd_packet);
      npu_data->instruction_buffer = bo_ctrl.paddr();
      npu_data->instruction_buffer_size = bo_ctrl.size();
      npu_data->instruction_prop_count = 0;
      inc_pkt_count(sizeof(*npu_data));
      break;
    }
    case ERT_START_DPU: {
      auto dpu_data = get_ert_dpu_data(cmd_packet);
      dpu_data->instruction_buffer = bo_ctrl.paddr();
      dpu_data->instruction_buffer_size = bo_ctrl.size();
      dpu_data->chained = 0;
      inc_pkt_count(sizeof(*dpu_data));
      break;
    }
    default:
      throw std::runtime_error("Unknown exec buf op code: " + std::to_string(m_op));
    }
  }

  void
  add_arg_32(uint32_t val)
  {
    inc_pkt_count(sizeof(val));
    auto args = get_ert_regmap_begin(m_cmd_pkt);
    args[m_reg_idx++] = val;
    m_arg_cnt++;
  }

  void
  add_arg_64(uint64_t val)
  {
    inc_pkt_count(sizeof(val));
    auto args = get_ert_regmap_begin(m_cmd_pkt);
    args[m_reg_idx++] = val;
    args[m_reg_idx++] = val >> 32;
    m_arg_cnt++;
  }

  void
  add_arg_bo(bo& bo_arg, const std::string arg_name="")
  {
    // Add to argument list for driver
    m_exec_buf_bo.get()->bind_at(m_arg_cnt, bo_arg.get(), 0, bo_arg.size());
    // Add to argument list for control code patching
    if (arg_name.empty())
      m_patching_args.emplace_back(std::to_string(m_arg_cnt), bo_arg.paddr());
    else
      m_patching_args.emplace_back(arg_name, bo_arg.paddr());
    // Only increase m_arg_cnt now after it's used by code above.
    add_arg_64(bo_arg.paddr());
  }

  void
  dump()
  {
    std::cout << "Dumping exec buf:";
    auto data = m_exec_buf_bo.map();
    std::cout << std::hex;
    for (int i = 0; i < m_cmd_pkt->count + 1; i++) {
      if (i % 4 == 0)
        std::cout << "\n";
      std::cout << std::setfill('0') << std::setw(8) << data[i] << " ";
    }
    std::cout << std::setfill(' ') << std::setw(0) << std::dec << std::endl;

    std::cout << "Dumping patching arguement list:\n";
    for (auto& [arg_name, arg_addr] : m_patching_args)
      std::cout << "{ " << arg_name << ", 0x" << std::hex << arg_addr << std::dec << " }\n";
  }

  static size_t
  get_ctrl_code_size(std::string elf_path)
  {
    auto elf = xrt::elf{elf_path};
    auto mod = xrt::module{elf};
    size_t instr_size = 0;
    xrt_core::module_int::patch(mod, nullptr, &instr_size, nullptr);
    return instr_size;
  }

  void
  patch_ctrl_code(bo& bo_ctrl, std::string elf_path)
  {
    auto elf = xrt::elf{elf_path};
    auto mod = xrt::module{elf};
    size_t instr_size = bo_ctrl.size();
    xrt_core::module_int::patch(mod, reinterpret_cast<uint8_t*>(bo_ctrl.map()), &instr_size, &m_patching_args);
    bo_ctrl.get()->sync(buffer_handle::direction::host2device, instr_size, 0);
  }

private:
  void
  inc_pkt_count(uint32_t n)
  {
    m_cmd_pkt->count += n / sizeof(int32_t);
    if (m_cmd_size < sizeof(m_cmd_pkt->header) + m_cmd_pkt->count * sizeof(int32_t))
      throw std::runtime_error("Size of exec buf too small: " + std::to_string(m_cmd_size));
  }

  bo& m_exec_buf_bo;
  ert_start_kernel_cmd *m_cmd_pkt;
  size_t m_cmd_size;
  uint32_t m_op;
  uint32_t m_arg_cnt;
  uint32_t m_reg_idx;
  std::vector< std::pair<std::string, uint64_t> > m_patching_args;
};

#endif // _SHIMTEST_EXEC_BUF_H_
