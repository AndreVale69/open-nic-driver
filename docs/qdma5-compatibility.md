# QDMA 5.0 compatibility for open-nic-driver

Branch `feat/qdma-v5-compat` of the DREAM-NIC forks of
[open-nic-driver](https://github.com/Xilinx/open-nic-driver) and
[open-nic-shell](https://github.com/Xilinx/open-nic-shell). Target: AMD Alveo
U250 (`xcu250-figd2104-2L-e`). Baseline: Vivado 2022.1 / QDMA 4.0. Additional
target: Vivado 2022.2 or later / QDMA 5.0 (tested with 2023.2).

## Status at a glance

| Item | Implemented | Tested without hardware | Verified on hardware |
| --- | --- | --- | --- |
| Host kernel 7.0 build fix (`__percpu`) | yes | module builds on 7.0.0-44 | no |
| QDMA generation detection (`0x0`, `0x134`) | yes | 12 decode cases + register-read test | **no** |
| Per-generation hw ops, QDMA 4.0 path unchanged | yes | full write sequence equals upstream `3de2269` | **no** |
| QDMA 5.0 global CSR programming | yes | every value checked against the v5 CSV layout; an independent review compiled REF `eqdma_set_perf_opt()` and got the same 15 writes for cache depths 8 to 128 | **no** |
| `qdma_ip_override` module parameter | yes | builds, `modinfo` lists it | **no** |
| `ethtool -i` reports QDMA generation | yes | builds | **no** |
| RX NAPI poll: budget contract, CIDX handed back on every poll | yes | `test_rx_poll` runs the real `onic_rx_poll()`, also in an event loop with PG302's interrupt and ring-full behaviour; the previous code overflows the completion ring there, which invalidates the queue | **no** |
| Zero-budget (netpoll) poll completes skbs only, no XDP | yes | `test_rx_poll` | **no** |
| Probe: FMAP timeout unwind, NULL-safe teardown, bounded CMAC reset wait | yes | `test_hw`; the previous code dereferences a NULL `qdma_dev` and hangs on a stuck CMAC reset | **no** |
| Error interrupt: H2C errors at global bit 16, decode with 4.0/5.0 names, clear and re-arm | yes | `test_hw`; `qdma_check_regs.py` checks the global bits | **no** |
| `tools/qdma_reg_dump.py` checks derived from the measured capabilities | yes | `test_reg_dump.py` on BAR images programmed by the driver code, cache depths 8 to 128 | **no** |
| Context, DMAP and descriptor encodings | unchanged, audited | checked against PG302 / CSV layouts | **no** |
| Shell: QDMA IP version resolve, enforce, record what the design project uses | yes | stale 2022.1 cache under 2023.2, reused project with other IP than the cache, fresh builds with 2022.1 and 2023.2, catalog mismatch | n/a |
| Shell RTL: C2H completion FIFO reset gate and ECC clock enable | yes | independent review's XSim benches under 2023.2 (reset window, ECC under backpressure, 65,544-packet scoreboard) | **no** |
| Shell RTL / IP port compatibility 4.0 vs 5.0 | no change needed | IP generated with both Vivados and diffed | **no** (no bitstream built) |

**Nothing here has been run on an FPGA.** A successful build and unit tests do
not show that QDMA 5.0 hardware works with this driver. The hardware
procedure in the last section is what will show that.

## Sources

| Tag | Source |
| --- | --- |
| CSV4 | `qdma_v4_0_pf_registers.csv` from `https://www.xilinx.com/support/documents/ip_documentation/qdma/v4_0/qdma-v4-0-register-map.zip` |
| CSV5 | `qdma_v5_0_pf_registers.csv` from `https://www.xilinx.com/support/documents/ip_documentation/qdma/v5_0/qdma-v5-0-register-map.zip` (readme v3.0, 19 Oct 2022), linked from [PG302 v5.0 QDMA_CSR](https://docs.amd.com/r/5.0-English/pg302-qdma/QDMA_CSR-0x0000) |
| PG5 | PG302 v5.0 pages [QDMA_TRQ_SEL_QUEUE_PF](https://docs.amd.com/r/5.0-English/pg302-qdma/QDMA_TRQ_SEL_QUEUE_PF-0x18000), [DMAP_SEL_CMPT_CIDX](https://docs.amd.com/r/5.0-English/pg302-qdma/QDMA_DMAP_SEL_CMPT_CIDX-2048-0x1800C), [DMAP_SEL_H2C_DSC_PIDX](https://docs.amd.com/r/5.0-English/pg302-qdma/QDMA_DMAP_SEL_H2C_DSC_PIDX-2048-0x18004), [PF mailbox](https://docs.amd.com/r/5.0-English/pg302-qdma/QDMA_PF_MAILBOX-0x22400) |
| REF | [Xilinx/dma_ip_drivers](https://github.com/Xilinx/dma_ip_drivers) `QDMA/linux-kernel/driver/libqdma/qdma_access`: release 2023.2.1 `a93d4a4` and current `a918c51` (2025.2) for QDMA 5.0, releases 2020.2 `5be3966` and 2022.1 `35276a7` for QDMA 4.0 |
| D46 | [open-nic-shell discussion #46](https://github.com/Xilinx/open-nic-shell/discussions/46) |
| PR69 | [open-nic-driver PR #69](https://github.com/Xilinx/open-nic-driver/pull/69) (open) |
| IPX | `xilinx.com:ip:qdma` generated from `qdma_no_sriov_au250.tcl` with Vivado 2022.1 and 2023.2 (this work) |

The CSVs are AMD material and are not committed. `tools/qdma_check_regs.py`
takes their paths as arguments.

## 1. Audit of the existing driver

Upstream `main` at `3de2269`, all upstream branches, and open issues and PRs
were searched. **No QDMA 5.0 support exists.** The only related code is unused
`QDMA_GLBL2_MISC_CAP` field macros in `qdma_register.h`. Those encode an older
layout (`EVEREST_IP` bit 28, `RTL_VERSION [23:16]`) that does not match REF's
decode.

How OpenNIC touches the QDMA:

* `onic_init_hardware()` maps BAR-0 (QDMA config) and BAR-2 (shell), writes
  the FMAP context, and on the master PF calls the global CSR init.
* The CSR init (`onic_qdma_init_csr()`, now `qdma_init_global_csr_v4()`)
  writes the ring size, buffer size, timer and threshold tables, then
  `0x250`, `0xB08`, `0xB50` and `0xE24`.
* Queue setup writes SW, HW, credit, prefetch and completion contexts through
  the indirect context registers `0x804`, `0x824` and `0x844`.
* The datapath writes DMAP PIDX/CIDX at `0x18000 + 16*qid`.
* MSI-X is direct, one vector per queue pair, with no interrupt aggregation,
  so the interrupt context is unused. An error interrupt is configured via
  `0xB04` plus the leaf masks.
* No FLR, mailbox, SR-IOV or MM-channel use.

The register definitions in `qdma_register.h` come from the QDMA 3.x soft IP
(REF `qdma_soft_reg.h`), not the EQDMA layout that both QDMA 4.0 and 5.0 use.
Most of them coincide. The exceptions (`0xB08`, `0xE24`) are listed below.

Differences from AMD's reference driver that matter here:

1. REF decides the IP from `MISC_CAP[23:20]` (0 = 3.x, 1 = 4.0, 2 = 5.0).
   OpenNIC did no detection.
2. REF skips `GLBL_DSC_CFG`, `PFCH_CFG_1/2` and `H2C_REQ_THROT` in its
   4.0 branch on 5.0, and runs `eqdma_set_perf_opt()` instead, which programs
   14 further registers. OpenNIC wrote a fixed set of values on every
   generation.
3. REF's comment in `qdma_fetch_version_details()`: *"For QDMA4.0 and QDMA5.0,
   HW design and register map is same except some performance
   optimizations"*. The REF context masks in `eqdma_soft_reg.h` are byte
   identical between 2020.2 and 2022.1. Between 2022.1 and 2025.2 that file
   only gains registers and debug fields, and no context field changes.
4. For QDMA 5.0, REF limits MM descriptors to < 64 KiB (`qdma_descq.c`). That
   is MM only; OpenNIC uses ST H2C/C2H with 16 bit lengths.

## 2. Compatibility matrix

"Affects OpenNIC" means OpenNIC programs or depends on the register.

### Global CSRs

| Offset | Register | QDMA 4.0 (CSV4) | QDMA 5.0 (CSV5) | Affects OpenNIC | Handling |
| --- | --- | --- | --- | --- | --- |
| `0x000` | Config_Block_Identifier | `identifier[31:20]=0x1fd`, `[19:16]=3` | same | yes (detection) | magic `0x1fd3` checked, probe fails with `-ENODEV` otherwise |
| `0x134` | GLBL2_MISC_CAP | opaque `misc_cap[31:0]` | same | yes (detection) | decoded per REF: `[31:28]` device type, `[27:24]` Vivado id, `[23:20]` IP version, `[19:16]` RTL version |
| `0x04C` | Config_Block_Misc_Control | `num_tag[19:8]=256`, `rq_metering_multiplier[4:0]=25` | same layout | v5 path | REF value on 5.0 only: `0x00010009` |
| `0x158`/`0x15C`/`0x160` | GLBL2_RRQ_{BRG,PCIE,AXIMM}_THROT | absent | **new**: `req_throt_en[31] req_throt[30:19] dat_throt_en[18] dat_throt[17:0]`; reset BRG `1/192/1/26624`, PCIE `1/192/1/20480`, AXIMM `0` | v5 path | REF values: PCIE `0x06045000`, AXIMM `0`, BRG `0x86045000` |
| `0x204..0x240` | GLBL_RNG_SZ_1..16 | `ring_size[15:0]` | same | yes | unchanged OpenNIC table |
| `0x250` | GLBL_DSC_CFG | `Max_dsc_fetch[5:3] wb_acc_int[2:0]`, `[31:10]` reserved | **adds `c2h_uodsc_limit[29:20]`, `h2c_uodsc_limit[19:10]`** (reset 0) | yes | 4.0: unchanged `0x30`. 5.0: REF `0x00502015` (uodsc 5/8, fetch 2, wb 128) |
| `0xA00`/`0xA40` | C2H_TIMER_CNT / C2H_CNT_TH | `[15:0]` | same | yes | unchanged OpenNIC tables |
| `0xA80` | C2H_PFCH_CFG_1 | `evt_qcnt_th[31:16]=56 pfch_qcnt[15:0]=60` | same | v5 path | 4.0: never written (QDMA 3.x had QID2VEC here). 5.0: `cache_depth-4` in both fields |
| `0xA84` | C2H_PFCH_CFG_2 | `fence[31] ll_sz_th[27:12] var_desc_num[11:6]=31 num_pfch[5:0]` | same layout, `var_desc_num` reset **15** | v5 path | 4.0: never written. 5.0: `0x804003C8` (fence on) |
| `0xAB0..0xAEC` | C2H_BUF_SZ | `[31:0]` | same | yes | unchanged OpenNIC table |
| `0xB04` | GLBL_ERR_INT | `Func[11:0] Vec[22:12] En_coal[23] Err_int_arm[24]` | same | yes | driver uses `Func[7:0]`, a sufficient subset |
| `0xB08` | C2H_PFCH_CFG | `evt_pfch_fl_th[31:16]=16 pfch_fl_th[15:0]=256` | same | yes | **driver used the QDMA 3.x layout on both**: writes `0x7D001100`, i.e. `pfch_fl_th=4352`, `evt_pfch_fl_th=32000`. 4.0: kept. 5.0: REF `0x01000100` |
| `0xB0C` | C2H_INT_TIMER_TICK | `[31:0]` | same | yes | unchanged (25) |
| `0xB50` | C2H_WRB_COAL_CFG | `max_buf_sz[31:26] tick_val[25:14] tick_cnt[13:2]` | same | yes | unchanged. Note: 128 written to the 6-bit `max_buf_sz` becomes 0 (REF does the same) |
| `0xBE0` | C2H_PFCH_CACHE_DEPTH | `MAX_STBUF[23:16] cache_depth[7:0]` | same | read | v5 path uses `cache_depth[7:0]` only, clamped to >= 8 |
| `0xE24` | H2C_REQ_THROT_PCIE | `en_req[31] req_throt[30:19] en_data[18] data_thresh[17:0]` | same | yes | **driver used the QDMA 3.x layout on both**: `0x00C14000` leaves both throttles disabled. 4.0: kept. 5.0: REF `0x86046000` |
| `0xE2C` | H2C_REQ_THROT_AXIMM | same fields, reset disabled | same fields, reset **enabled** `1/0x40/1/0x4000` | no MM | 5.0: REF `0x82044000` |
| `0x12EC` | H2C_MM_Data_Throttle | absent | **new** `dat_throt_en[16] dat_throt[15:0]` | no MM | 5.0: REF `0x00015000` |
| `0x1400`/`0x1404` | C2H_CRDT_COAL_CFG_1/2 | `pld_fifo_th[17:10] crdt_timer_th[9:0]` / `crdt_fifo_th[23:16] crdt_cnt_th[10:0]` | same | v5 path | 5.0: `0x4010`, `(cache_depth-8)<<16 | 96` |
| `0x147C`/`0x1484` | PFCH_CFG_3 / PFCH_CFG_4 | absent | **new** | v5 path | 5.0: `0x8000`, `0x00800320` |
| `0x1410..0x1484` | stats and debug | v4 has them at `0x1500`, `0x1800..0x190C` | **moved** to `0x1410..0x1478` | no | not used |
| `0x248`/`0x24C` | GLBL_ERR_STAT / MASK | `err_h2c_st[16]`, `err_c2h_st[8]`, `err_trq[3]`, `err_dsc[2]`, `err_ram_dbe[1]`, `err_ram_sbe[0]` | same | yes | **driver used the QDMA 3.x position bit 11 for H2C-ST**, which is part of `ind_ctxt_cmd_err[14:9]` on 4.0/5.0: H2C stream errors were never enabled and the handler skipped them. Now bit 16 (REF `eqdma_soft_reg.h`) |
| `0x254`/`0x264`/`0xAF0`/`0xAF8`/`0xE00`/`0xF4`/`0xFC` | error status/mask | identical in 4.0 and 5.0 | same | yes | Leaf enable masks unchanged: REF's QDMA 3.x ones (`qdma_soft_reg.h`), with a few undocumented bits (DSC bit 0, TRQ bit 2, C2H bit 11) and without some 4.0/5.0 sources (e.g. DSC `tag`, `flr`), identically on both. REF's EQDMA code uses other masks (e.g. C2H `0x3F6DF`), not adopted. The per-error names of `qdma_error_info.h` are the 3.x layout too (wrong for DSC, TRQ, H2C and RAM on 4.0/5.0), so the error handler names bits from the 4.0/5.0 CSVs. Status bits latch either way and `qdma_reg_dump.py` shows them |

### Contexts, queues, descriptors, interrupts, BARs, reset

| Area | QDMA 4.0 vs 5.0 | OpenNIC encoding | Affects OpenNIC |
| --- | --- | --- | --- |
| Indirect context command `0x844` | identical: `qid[19:7] op[6:5] sel[4:1] busy[0]` (CSV4/5) | `qid` 11 bits, enough for 2048 queues | no change |
| SW context | identical (REF) | `fnc_id` 8 of 12 bits, `fetch_max` 3 of 4 bits (value 0) | no change |
| HW / credit context | identical | clear/invalidate only | no change |
| Prefetch context | identical. EQDMA fields `num_pfch`, `pfch_need`, `var_desc`, `virtio` live in bits OpenNIC leaves 0 | `bufsz_idx`, `pfch_en`, `valid` | no change (REF also leaves them 0 for ST) |
| Completion context | identical. `baddr[37:6]` in W1, `[63:38]` in W2, `[5:2]` in W5 | older encoding drops `baddr[11:6]`, so the ring must be 4 KiB aligned. OpenNIC uses page-aligned `dma_alloc_coherent()` | no change, test documents the constraint |
| Interrupt context | identical | unused (no aggregation) | none |
| FMAP | indirect `sel=12`, `W0 qid_base[10:0]`, `W1 qid_max[11:0]` | same | no change |
| DMAP PIDX/CIDX | `0x18000..0x1800C`, stride 16, same bit fields (CSV4, PG5). CSV5 omits the queue space; PG5 documents it unchanged | same | no change. PG5's quoted end address (`0x1CFF0`) is inconsistent with 2048 x 16 B; the driver and REF use base + 16 x qid |
| ST descriptors / CMPT / status writeback | identical (H2C 16 B, C2H 8 B, CMPT 8 B) | same | no change |
| BARs | IPX: identical BAR/MSI-X parameters (MSI-X table size field PF0 `009`, PF1 `008`, i.e. 10 and 9 vectors; BAR2 4 MiB) | BAR-0 QDMA, BAR-2 shell, hard-coded. The master PF keeps 2 vectors (user, error), other PFs 1 | no change |
| Reset / FLR | OpenNIC does no QDMA soft reset or FLR. Its unused `0x2500` FLR macro is a QDMA 3.x address (EQDMA: `0x22500`) | n/a | none |
| Mailbox | PF mailbox at `0x22400` (PG5) | unused | none |

### FPGA side (IPX)

| Check | Result |
| --- | --- |
| IP version picked by `create_ip -name qdma` (no `-version`) | 2022.1 -> `qdma:4.0`, 2023.2 -> `qdma:5.0`. Each release ships only one, so 2023.2 cannot build QDMA 4.0 |
| CONFIG parameters | 835 (4.0) vs 864 (5.0): **no shared parameter changed value**. 5.0 adds `VFG*_MSIX_*`, `PL_DISABLE_LANE_REVERSAL*`, `enable_64bit`, `two/three_port_switch`, `axisten_if_enable_msg_route_override`, `all_speeds_all_sides`, `enable_pcie_debug_ports`, `pf0_aer_cap_ecrc_gen_and_check_capable` at defaults |
| Instantiated ports | 152 = 152, identical names, directions and widths. Optional ports differ (`usr_flr_clr`, `cfg_wrreq_flr_done` 4.0 only, `cfg_local_error_valid_sd` 5.0 only), but none is enabled in this configuration or used by the wrapper |
| `qdma_subsystem_*` RTL | no change needed for 5.0. Two generation-independent fixes in `qdma_subsystem_c2h.sv`: packets are not accepted while the completion FIFO is in reset (they lost their completion and shifted every later one), and the C2H control ECC is loaded with its beat (it changed under backpressure; only used with data protection, which the U250 XCI disables) |
| Build scripts | `-qdma_ip_version`, a log line and `build/<name>/QDMA_IP_VERSION` added (commit `da66649`). The version is now checked on the QDMA instances the design uses, not only the catalog: a build directory from another Vivado release keeps its old, locked QDMA IP and used to pass as 5.0 |

## 3. Why OpenNIC may stall on QDMA 5.0, and what this does and does not fix

D46 says the driver "can stop responding at high throughput" on 5.0 but gives
no register-level cause. The analysis supports two candidate causes, and
which one applies can only be decided on hardware:

1. **QDMA 5.0 global CSRs (addressed here).** On 5.0 the driver left the new
   descriptor-engine limits at 0, never configured the prefetch and credit
   coalescing registers, and wrote `0xB08`/`0xE24` with the QDMA 3.x layout.
   That disables H2C request throttling, which REF enables on 5.0. The 5.0
   path now applies REF's validated 5.0 programming. The same 3.x encodings
   are also active on 4.0 but are kept there, since that configuration is the
   validated baseline.
2. **Host-side NAPI/CIDX bug (fixed here, independent of QDMA version).**
   In `onic_rx_poll()`, when the budget was exhausted the driver called
   `napi_complete()` and then `napi_schedule()`, and skipped
   `onic_set_completion_tail()`. The CIDX was written only by a poll that
   drained the ring up to the PIDX it read at its start. Once every poll
   starts with at least a budget (64) of completions, the QDMA gets no
   completion credit back, and the ring (1022 completions at most, PG302)
   fills within 16 polls. PG302 ("Handling Exception Events"): a completion
   for a full ring is dropped, the queue's completion context is invalidated
   and marked with error, `wrb_qfull_err` (`0xAF0` bit 13) is logged, and
   completions for an invalid context are then dropped silently. RX on that
   queue stays dead until the queue is set up again (interface down/up).
   `test_rx_poll` runs the old code in an event loop with that behaviour:
   at 64 or 100 arrivals per poll the ring overflows once and the queue is
   lost (1022 of 19200 packets delivered). PR69 reports an RX stall above
   2 Gbps; that PR rewrites the whole file and is not merged here.

   The fix is minimal and follows the NAPI contract and REF
   (`descq_process_completion_st_c2h()` writes the CIDX after every batch):
   * every poll that runs RX hands the consumed entries back (CIDX);
   * budget used: CIDX written, no interrupt requested, return the budget,
     leave the NAPI state alone;
   * below budget: `napi_complete_done()`, and request the interrupt only if
     it returns true;
   * budget 0 (netpoll): skb TX completions only, no RX, XDP or page pool.

   With the CIDX returned every poll, the QDMA can have at most the posted
   RX descriptors (the driver keeps 128 to 383 posted) plus one poll (64) of
   completions outstanding, well below 1022, so the ring cannot overflow
   while every completion uses a descriptor (packets up to the 4 KiB
   buffer). More traffic than a queue takes is dropped by the QDMA for lack
   of a descriptor instead (`0xB10` counter), and the queue keeps working;
   the event loop checks this for every case.

   Interrupts: the CIDX update carries an interrupt request (`irq_en_wrb`),
   but the completion context is programmed with `full_upd` = 0 (as REF does
   without adaptive RX), so the QDMA copies only the CIDX (PG302,
   completion context) and takes every CIDX update as servicing the
   outstanding interrupt, then re-evaluates its trigger. Under sustained
   load that means about one interrupt per poll; NAPI absorbs it (an
   interrupt during a poll only marks NAPI as missed). The event loop shows
   no lost wakeup or overflow either way; with `full_upd` = 1 the requests
   would take effect (one interrupt per burst instead of per poll), which is
   a tuning option to try on hardware, not a fix.

   It also stops the RX poll from writing 0 to `QDMA_GLBL_ERR_INT` on a
   completion with the error bit, which disabled the master PF's error
   interrupt from any queue. Whether this was *the* stall on QDMA 5.0 can
   only be decided on hardware; the plan below separates the causes.

## 4. Changes

open-nic-driver, branch `feat/qdma-v5-compat` on top of upstream `3de2269`:

| Commit | Summary |
| --- | --- |
| `a581c62` | fix(build): `__percpu` on `netdev_stats`. Host kernel 7.0 only, not QDMA |
| `cd17853` | refactor: CSR init moved to `qdma_access/qdma_csr.c`, no functional change |
| `97ba662` | feat: `qdma_version.[ch]`, `qdma_register_v5.h`, `struct qdma_hw_ops`, `qdma_init_global_csr_v5()`, `qdma_ip_override` |
| `86caf16` | feat: `ethtool -i` `fw_version` = detected generation and selected programming |
| `10a41a7` | test: userspace harness, 284 checks, `make check` |
| `b595d33` | tools: `qdma_check_regs.py` |
| `ec13b07` | docs and `tools/qdma_reg_dump.py` |
| follow-up | fix(netdev): RX NAPI budget contract and per-poll CIDX; a zero-budget poll completes skbs only; completion error counted (`rx_errors`) instead of disabling the error interrupt; unused `tx_ring` (`W=1`) |
| follow-up | fix(hw): probe unwind on FMAP timeout, NULL-safe `onic_clear_hardware()`, CMAC reset wait bounded to 1 s |
| follow-up | fix(qdma): H2C stream errors enabled at global bit 16; the error interrupt is decoded with the 4.0/5.0 names, cleared and re-armed |
| follow-up | test: `drvshim/` kernel shim, `test_rx_poll` (with a PG302 event loop), `test_hw`; fake BAR-2 and stuck-busy knob in `fake_mmio.c` |
| follow-up | tools: `qdma_reg_dump.py` reads the magic first and only registers of the detected generation, derives expectations from `0xBE0`/`0xBE4`/`0x15C`, follows `qdma_ip_override`, decodes error status, shows the C2H counters (`gen_bar_image` + `test_reg_dump.py`); `qdma_check_regs.py` checks the global error bits and fails on undefined error-mask bits other than the known ones |

open-nic-shell, branch `feat/qdma-v5-compat` on top of `940907f`:
`da66649` resolve, record and optionally enforce the QDMA IP version.
Follow-up: check the version on the QDMA instances of the IP build directory
and of the design project, also a reused one, refuse locked IP when a
version is requested, and write `QDMA_IP_VERSION` from the design project
only after the check (VLNV, Vivado version, one line per instance); warn
when an existing project makes `-impl` a no-op; `-impl_strategy`; C2H
completion FIFO reset gate and ECC clock enable.

Design notes:

* The abstraction is one ops table with one operation, because the only
  generation-dependent behaviour is global CSR programming. Contexts,
  descriptors and DMAP stay shared, so there are no version checks scattered
  through the datapath.
* Unknown or QDMA 3.x hardware keeps the QDMA 4.0 path, with a warning. That
  is what the driver did before detection existed.
* Only the 5.0 constants were adapted from REF (`eqdma_set_perf_opt()` in the
  Linux driver, GPL-2.0 like open-nic-driver). No REF code was copied. OpenNIC keeps its own ring and
  buffer tables, because `onic_netdev.c` indexes them.

## 5. Build and test results (host: Ubuntu 26.04, kernel 7.0.0-44-generic, GCC 15.2)

| Check | Result |
| --- | --- |
| Upstream `3de2269` on kernel 7.0 | **fails**: `onic_main.c:236 assignment from pointer to non-enclosed address space` (host-kernel issue, fixed in `a581c62`) |
| `make` (kernel module, `-Werror`) | passes |
| `make W=1` (with the normal `-Werror`) | passes (failed at `ec13b07` on the unused `tx_ring`) |
| `make -C tests check` | `test_qdma` 284 checks, `test_rx_poll` 4658 (the event loop checks every poll), `test_hw` 68, `test_reg_dump.py` 9 tests: 0 failures |
| `make -C tests check-asan` (ASan + UBSan + LeakSanitizer) | same, 0 failures, no leaks |
| New tests against the `ec13b07` sources | `test_rx_poll`: 54 failures. Full budget writes no CIDX and calls `napi_complete()` + `napi_schedule()`; budget 0 processes a packet and returns an XDP frame; in the PG302 event loop at 64 and 100 arrivals per poll the ring overflows, the context is invalidated and the queue is lost (1022 of 19200 delivered); an skb failure leaves NAPI scheduled forever. `test_hw`: UBSan stops on a member access through a NULL `struct qdma_dev` (`qdma_context.c:446`) after the FMAP timeout; a stuck CMAC reset never returns |
| Event loop, this branch (300 rounds, both `full_upd` settings) | no overflow, nothing lost, NAPI idle and every completion handed back at the end; `full_upd` = 0: about one interrupt per poll under load, `full_upd` = 1: one per burst |
| Mutation check (QDMA 4.0 constant, QDMA 5.0 fence bit, ops selection) | all three detected |
| `tools/qdma_check_regs.py` against CSV4 and CSV5 | OK 138, SUBSET 4, LEGACY 20 (14 field layouts, 6 known error-mask bits), unexpected 0; an added undefined mask bit and the old H2C-ST bit 11 each fail it |
| `tools/qdma_reg_dump.py --resource` on BAR images programmed by the driver code | 0 mismatches for every generation x `0xBE0` in {8 .. 128, with MAX_STBUF} x `0xBE4` in {16, 32, 128}; wrong programming, no magic and a hand-edited `0x158` are flagged. The `ec13b07` dump flagged a correct depth-16 image |
| clang build | not run (clang not installed) |
| sparse | not run (not installed) |

Host-kernel issues are only the `__percpu` annotation, which does not touch
QDMA programming.

## 6. Hardware validation procedure

Nothing in this section has been done. The steps marked **root** program
the FPGA, load modules or read BARs, so they need the operator.

### 6.0 Setup

1. Build two bitstreams from the same shell commit, each in a **new build
   directory** (`-tag`) per Vivado release and configuration:
   * A: Vivado 2022.1, `-qdma_ip_version 4.0`
   * B: Vivado 2023.2, `-qdma_ip_version 5.0`

   Keep `build/*/QDMA_IP_VERSION`, the Vivado logs, the post-route timing
   summary and the bitstream SHA-256. Only take an image whose routed timing
   closes (WNS and WHS >= 0) and whose `write_bitstream` passed (the CMAC
   needs AMD's free UltraScale+ 100G Ethernet license on the build host).
   With Vivado 2023.2 the stock 1-PF U250 shell missed setup timing with the
   default strategy (WNS -0.407 ns, CMAC 322 MHz and QDMA H2C aligner paths,
   mostly routing delay). From the same opt checkpoint, `place_design
   -directive Explore` with AggressiveExplore phys_opt and routing closed it
   (WNS +0.100 ns, WHS +0.008 ns); post-route phys_opt alone did not. That
   checkpoint predates the C2H RTL fixes and used more aggressive directives
   than the named strategy. Build B with `-impl_strategy
   Performance_ExplorePostRoutePhysOpt` (Explore for every step) and check
   its own report; the final 2-PF design with the filter was not
   implemented. `build.tcl` refuses a QDMA instance of the wrong version in
   the IP build or the design project, also a reused one, but use a fresh
   `-tag` per image anyway and keep the `QDMA_IP_VERSION` it writes.
2. Build three drivers for the lab kernel and record `git describe`,
   `uname -r` and `modinfo ./onic.ko` for each:
   * this branch;
   * `ec13b07`: QDMA 5.0 programming, without the RX completion fix;
   * `a581c62`: upstream `3de2269` plus the kernel 7.0 build fix, the legacy
     driver (QDMA 4.0 programming on every generation, no detection, no
     `qdma_ip_override`).
3. Traffic: either a second host on the 100G link, or the U250 looped to a
   NIC on the same host in another network namespace. Note the link partner
   and its RS-FEC setting.
4. Run every step below for each of these combinations, in this order if
   board time is short (1 to 3 separate the two causes on the target
   hardware). The programming paths write different register sets and
   module unload resets none of them, so **reprogram the FPGA before each
   combination** (or at least whenever the programming changes) and keep a
   dump from the start:

| # | Bitstream | Driver | `qdma_ip_override` | Isolates |
| --- | --- | --- | --- | --- |
| 1 | B (5.0) | this branch | 0 | the target: 5.0 programming + RX fix |
| 2 | B (5.0) | `ec13b07` | 0 | 5.0 programming without the RX fix |
| 3 | B (5.0) | `a581c62` | n/a | the legacy driver on 5.0 hardware (the reported stall) |
| 4 | A (4.0) | this branch | 0 | QDMA 4.0 baseline with the RX fix |
| 5 | B (5.0) | this branch | 4 | 4.0 programming on 5.0 hardware with the RX fix |
| 6 | A (4.0) | `a581c62` | n/a | the legacy driver on 4.0 hardware |

`qdma_reg_dump.py --expect auto` follows `qdma_ip_override`, which
`a581c62` does not have: use `--expect v4` for combinations 3 and 6.

### 6.1 PCIe enumeration

```bash
lspci -d 10ee: -nn                     # PF0 903f / PF1 913f (U250 tcl)
sudo lspci -d 10ee: -vvv | grep -E 'LnkSta:|Region|MSI-X'   # root
```

Pass: Gen3 x16 (`8GT/s, Width x16`), BAR0 present, BAR2 = 4M, MSI-X
`Count=10` on PF0 and `Count=9` on PF1 (the table size field is N-1). A
one-PF image has no PF1. Record `Allocated N queue vectors` from dmesg: the
driver may get fewer and keeps 2 (PF0) or 1 (PF1) for non-queue use.

### 6.2 Driver initialisation (root)

```bash
sudo insmod ./onic.ko qdma_ip_override=<0|4|5>
sudo dmesg | grep -E 'onic|QDMA'
ethtool -i <iface>                      # fw-version: "QDMA 5.0, prog QDMA 5.0"
sudo python3 -I tools/qdma_reg_dump.py <bdf of PF0> --expect auto
```

Pass:
* dmesg shows `QDMA IP: QDMA 4.0` (A) or `QDMA 5.0` (B), and the `misc_cap`
  value. **Record the raw `misc_cap`**: real values have never been observed.
* No context programming timeouts (`-EBUSY`).
* `qdma_reg_dump.py` reports 0 mismatches for the selected programming.
  It derives the expected values from the cache depths this image reports
  (16 on the U250 build) and reads `qdma_ip_override` from sysfs, so
  `--expect auto` is right for every combination with this branch. Values
  that read back differently from what was written are a hardware finding in
  their own right (for example a field narrower than documented).
* No `QDMA error:` lines in dmesg. With this branch each error interrupt
  logs the global and leaf status with names, clears it and re-arms.

### 6.3 Basic H2C / C2H and packet correctness

```bash
sudo ip addr add 10.10.99.1/24 dev <iface>; sudo ip link set <iface> up
ping -c 1000 -i 0.002 -s 1472 -p a5 10.10.99.2       # pattern payload
ping -c 200 -s 8 10.10.99.2; ping -c 200 -s 1472 10.10.99.2
ethtool -S <iface> | grep -E 'rx_|tx_'
```

Pass: 0 loss, replies match the payload pattern, and the host counters
match the shell's CMAC/QDMA counters within the test traffic. For a byte-exact
check, capture with `tcpdump -w` on both ends and compare payload hashes of
an `iperf3 --bidir` run, or a scapy sender/receiver with sequence numbers and
CRC32 per payload.

### 6.4 Interrupts and queue operation

```bash
grep onic /proc/interrupts            # per-queue vectors and error vector
iperf3 -c 10.10.99.2 -P 16 -t 30      # many flows -> all RSS queues
grep onic /proc/interrupts
```

Pass: every queue's vector count increases under inbound traffic (vary the
flows until each queue vector moves; `ethtool -S` only has aggregate and XDP
counters, not per-queue packet counts), the error-interrupt vector stays 0,
and `QDMA_GLBL_ERR_STAT` (`0x248`), `C2H_ERR_STAT` (`0xAF0`),
`C2H_FATAL_ERR_STAT` (`0xAF8`) and `H2C_ERR_STAT` (`0xE00`) read 0 in
`qdma_reg_dump.py` afterwards (it names any bit that is set).

### 6.5 Repeated initialisation and reset (root)

```bash
for i in $(seq 50); do
  sudo rmmod onic && sudo insmod ./onic.ko qdma_ip_override=<n> || break
  sudo ip link set <iface> up; ping -c 3 -W 1 10.10.99.2 || echo "fail $i"
done
for i in $(seq 100); do sudo ip link set <iface> down; sudo ip link set <iface> up; done
```

Pass: every iteration pings. No context timeouts, no `WARN`/`BUG`, and no
leaked IRQs (`/proc/interrupts` back to the same lines). Register dump is
identical after the last iteration.

### 6.6 Sustained high throughput

```bash
# same loads as PR #69, then long runs
iperf3 -c 10.10.99.2 -u -b 4G -l 64   -P 16 -t 120
iperf3 -c 10.10.99.2 -u -b 4G -l 1450 -P 16 -t 120
iperf3 -c 10.10.99.2 -P 16 -t 1800          # TCP, 30 min, both directions (-R)
watch -n 10 'ethtool -S <iface> | grep -E "rx_packets|tx_packets"'
```

Pass: counters keep increasing for the whole run; throughput does not
collapse to 0; `C2H_ERR_STAT` bits 13 (`wrb_qfull_err`) and 14
(`wrb_cidx_err`) stay 0; the interface still pings afterwards without a
reload. Record offered and received load, not only the receiver side.

If it stalls: before reloading anything, save `dmesg`, `/proc/interrupts`,
`ip -s link` (`rx_errors` counts completions with the error bit) and a
`qdma_reg_dump.py` run, then stop the generator and send one ping to see if
the queue recovers on its own. A `wrb_qfull_err` means the queue's
completion context has been invalidated (PG302): that queue receives
nothing until the interface goes down and up. Loss without a stall is
different: compare the C2H counters in two dumps; `0xB10`
(`DESC_RSP_DROP_ACCEPTED`) counts packets dropped because no RX descriptor
was posted, which this driver allows beyond 128 to 383 descriptors.

### 6.7 Interpreting the A/B matrix

* 3 and 2 stall, 1 does not: the RX completion fix cured it (cause 2).
  Check that the stalled captures show `wrb_qfull_err`.
* 3 stalls, 2 does not: the QDMA 5.0 CSR programming cured it (cause 1).
* 1 stalls with `wrb_qfull_err`/`wrb_cidx_err`: the host still returns a
  wrong or late CIDX. Check the CPU load of the RX softirqs
  (`mpstat -P ALL 1`) and how many queues carry the load; a build with
  `debug = 1` in `onic_rx_poll()` logs the CIDX of every poll (slow, short
  runs only). Without those bits, look at the other named errors first.
* 1 and 4 both stall: a cause shared by both generations (host, shell RTL or
  traffic setup), not QDMA 5.0.
* 4 passes, 1 fails at init (6.2): look at the `misc_cap` decode and the
  register read-back first.
* Only call QDMA 5.0 "verified on hardware" if 6.1 to 6.6 all pass on
  combination 1, and only for that bitstream, driver, kernel and card.

## 7. Known issues found during the audit (not changed)

* QDMA 4.0 path: the `0xB08`/`0xE24` QDMA 3.x encodings and the undocumented
  error-mask bits are kept to preserve the baseline. Changing them needs a
  4.0 hardware test.
* `C2H_WRB_COAL_CFG.max_buf_sz` receives the buffer depth in a 6-bit field:
  128 becomes 0, 16 stays 16. CSV5 defines 0 as the configured maximum.
* The leaf error enable masks are the QDMA 3.x ones on 4.0 and 5.0
  (section 2): some 4.0/5.0 error sources never raise the error interrupt.
  Their status bits still latch; `qdma_reg_dump.py` shows them.
* RX keeps only 128 to 383 of the 1023 descriptors posted (refill by 256
  below 128), so a burst larger than that between two polls is dropped by
  the QDMA (`0xB10`). A throughput limit, not a stall.
* The completion context has `full_upd` = 0, so the interrupt requests in
  the CIDX updates are ignored and the QDMA can interrupt once per poll
  under load (section 3). `full_upd` = 1 would make them count; to be tried
  on hardware.
* PG302's completion context table lists trigger mode 5 as reserved, its
  CIDX register table as User_Timer_Count; OpenNIC and REF
  (`TRIG_MODE_COMBO`) use 5.
* `onic_xmit_frame()` also cleans TX, including XDP frames; netpoll calls it
  with interrupts disabled (matters only for netconsole on an onic port).
* RX descriptors point 256 B into a 4 KiB page but the C2H buffer size is
  4096 B: a packet longer than 3840 B would overrun the page. The shell's
  default `-max_pkt_len` is 1518.
* `onic_rx_page_refill()` does not check `page_pool_dev_alloc_pages()` for
  NULL, so RX can crash under memory pressure.
* An skb allocation failure in RX keeps the completion and retries it on the
  next interrupt, which runs an XDP program on that packet again.
* `onic_rx_poll()` reads completion entries after the status PIDX without a
  `dma_rmb()`: fine on x86, not on weakly ordered CPUs.
* `onic_clear_rx_queue()` frees the RX buffers with the completion ring's
  count; it equals the descriptor ring's (1024) today.
* No FLR or device reset recovery: a card reset or re-enumeration needs the
  module reloaded.
