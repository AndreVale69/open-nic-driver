# QDMA 5.0 compatibility for open-nic-driver

Branch `feat/qdma-v5-compat` of the DREAM-NIC forks of
[open-nic-driver](https://github.com/Xilinx/open-nic-driver) and
[open-nic-shell](https://github.com/Xilinx/open-nic-shell). Target: AMD Alveo
U250 (`xcu250-figd2104-2L-e`). Baseline: Vivado 2022.1 / QDMA 4.0. Additional
target: Vivado 2022.2 or later / QDMA 5.0 (tested with 2023.2).

## Status at a glance

| Item | Implemented | Tested without hardware | Verified on hardware |
| --- | --- | --- | --- |
| Host kernel 7.0 build fix (`__percpu`) | yes | module builds on 7.0.0-43 | no |
| QDMA generation detection (`0x0`, `0x134`) | yes | 12 decode cases + register-read test | **no** |
| Per-generation hw ops, QDMA 4.0 path unchanged | yes | full write sequence equals upstream `3de2269` | **no** |
| QDMA 5.0 global CSR programming | yes | every value checked against the v5 CSV layout | **no** |
| `qdma_ip_override` module parameter | yes | builds, `modinfo` lists it | **no** |
| `ethtool -i` reports QDMA generation | yes | builds | **no** |
| Context, DMAP, error-interrupt and descriptor encodings | unchanged, audited | checked against PG302 / CSV layouts | **no** |
| Shell: QDMA IP version resolve, record, enforce | yes | all 6 combinations of {2022.1, 2023.2} x {unset, 4.0, 5.0} | n/a |
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
| `0x254`/`0x264`/`0xAF0`/`0xAF8`/`0xE00`/`0xF4`/`0xFC` | error status/mask | identical in 4.0 and 5.0 | same | yes | unchanged. Driver masks enable a few undocumented bits (DSC bit 0, TRQ bit 2, C2H bit 11), identically on both |

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
| BARs | IPX: identical BAR/MSI-X parameters (PF0 9 vectors, BAR2 4 MiB) | BAR-0 QDMA, BAR-2 shell, hard-coded | no change |
| Reset / FLR | OpenNIC does no QDMA soft reset or FLR. Its unused `0x2500` FLR macro is a QDMA 3.x address (EQDMA: `0x22500`) | n/a | none |
| Mailbox | PF mailbox at `0x22400` (PG5) | unused | none |

### FPGA side (IPX)

| Check | Result |
| --- | --- |
| IP version picked by `create_ip -name qdma` (no `-version`) | 2022.1 -> `qdma:4.0`, 2023.2 -> `qdma:5.0`. Each release ships only one, so 2023.2 cannot build QDMA 4.0 |
| CONFIG parameters | 835 (4.0) vs 864 (5.0): **no shared parameter changed value**. 5.0 adds `VFG*_MSIX_*`, `PL_DISABLE_LANE_REVERSAL*`, `enable_64bit`, `two/three_port_switch`, `axisten_if_enable_msg_route_override`, `all_speeds_all_sides`, `enable_pcie_debug_ports`, `pf0_aer_cap_ecrc_gen_and_check_capable` at defaults |
| Instantiated ports | 152 = 152, identical names, directions and widths. Optional ports differ (`usr_flr_clr`, `cfg_wrreq_flr_done` 4.0 only, `cfg_local_error_valid_sd` 5.0 only), but none is enabled in this configuration or used by the wrapper |
| `qdma_subsystem_*` RTL | no change justified |
| Build scripts | `-qdma_ip_version`, a log line and `build/<name>/QDMA_IP_VERSION` added (commit `da66649`) |

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
2. **Host-side NAPI/CIDX bug (not addressed, independent of QDMA version).**
   In `onic_rx_poll()`, when the budget is exhausted the driver calls
   `napi_complete()` and then `napi_schedule()`, and skips
   `onic_set_completion_tail()`. So under sustained load the completion CIDX
   is only advanced when the ring drains. PR69 reports exactly this: an RX
   stall above 2 Gbps. That PR also rewrites the whole file, so it is not
   merged here. The hardware plan below is built to separate cause 1 from
   cause 2.

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
| (this commit) | docs and `tools/qdma_reg_dump.py` |

open-nic-shell, branch `feat/qdma-v5-compat` on top of `940907f`:
`da66649` resolve, record and optionally enforce the QDMA IP version.

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

## 5. Build and test results (host: Ubuntu 26.04, kernel 7.0.0-43-generic, GCC 15.2)

| Check | Result |
| --- | --- |
| Upstream `3de2269` on kernel 7.0 | **fails**: `onic_main.c:236 assignment from pointer to non-enclosed address space` (host-kernel issue, fixed in `a581c62`) |
| `make` (kernel module, `-Werror`) at every commit of the branch | passes |
| `make W=1` (without `-Werror`) | 1 warning, pre-existing: `onic_netdev.c:1196 tx_ring set but not used`. 0 in new or changed files |
| `checkpatch.pl --strict` on new files and the diff | 0 errors. Only missing-SPDX warnings, matching every existing file |
| `make check` / `make -C tests check-asan` (ASan + UBSan) | 284 checks, 0 failures |
| Mutation check (QDMA 4.0 constant, QDMA 5.0 fence bit, ops selection) | all three detected |
| `tools/qdma_check_regs.py` against CSV4 and CSV5 | OK 126, SUBSET 4, LEGACY 20, unexpected 0 |
| `tools/qdma_reg_dump.py --resource` on synthetic BAR images | expected v5 image passes; an image with the old `0x250` is flagged |
| clang build | not run (clang not installed) |
| sparse | not run (not installed) |

Host-kernel issues are only the `__percpu` annotation and the pre-existing
`W=1` warning. Neither touches QDMA programming.

## 6. Hardware validation procedure

Nothing in this section has been done. The steps marked **root** program
the FPGA, load modules or read BARs, so they need the operator.

### 6.0 Setup

1. Build two bitstreams from the same shell commit:
   * A: Vivado 2022.1, `-qdma_ip_version 4.0`
   * B: Vivado 2023.2, `-qdma_ip_version 5.0`

   Keep `build/*/QDMA_IP_VERSION` and the Vivado logs.
2. Build `onic.ko` from `feat/qdma-v5-compat`. Record `git describe`,
   `uname -r` and `modinfo ./onic.ko`.
3. Traffic: either a second host on the 100G link, or the U250 looped to a
   NIC on the same host in another network namespace. Note the link partner
   and its RS-FEC setting.
4. Run every step below for each of these combinations. Combination 4
   reproduces the old driver on new hardware:

| # | Bitstream | `qdma_ip_override` | Expected programming |
| --- | --- | --- | --- |
| 1 | A (4.0) | 0 | QDMA 4.0 (baseline) |
| 2 | A (4.0) | 4 | QDMA 4.0 |
| 3 | B (5.0) | 0 | QDMA 5.0 |
| 4 | B (5.0) | 4 | QDMA 4.0 on QDMA 5.0 hardware (old behaviour) |
| 5 | B (5.0) | 5 | QDMA 5.0 |

### 6.1 PCIe enumeration

```bash
lspci -d 10ee: -nn                     # PF0 903f / PF1 913f (U250 tcl)
sudo lspci -d 10ee: -vvv | grep -E 'LnkSta:|Region|MSI-X'   # root
```

Pass: Gen3 x16 (`8GT/s, Width x16`), BAR0 present, BAR2 = 4M, PF0 MSI-X
count 9.

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
  Values that read back differently from what was written are a hardware
  finding in their own right (for example a field narrower than documented).

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

Pass: every queue's vector count increases, the error-interrupt vector
stays 0, and per-queue counters in `ethtool -S` show traffic on all
configured queues. `QDMA_GLBL_ERR_STAT` (`0x248`), `C2H_ERR_STAT` (`0xAF0`),
`C2H_FATAL_ERR_STAT` (`0xAF8`) and `H2C_ERR_STAT` (`0xE00`) read 0 in
`qdma_reg_dump.py` afterwards.

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
reload.

### 6.7 Interpreting the A/B matrix

* 3 and 5 pass, 4 stalls: the QDMA 5.0 CSR programming fixed the stall
  (cause 1).
* 1, 3 and 4 all stall at high PPS: the stall is the NAPI/CIDX issue
  (cause 2, PR69), not QDMA 5.0. Fix `onic_rx_poll()`: write CIDX on budget
  exhaustion and do not complete NAPI when `work == budget`. Then retest.
* 1 passes, 3 fails at init (6.2): look at the `misc_cap` decode and the
  register read-back first.
* Only call QDMA 5.0 "verified on hardware" if 6.1 to 6.6 all pass on
  combination 3.

## 7. Known issues found during the audit (not changed)

* `onic_rx_poll()` NAPI/CIDX handling (section 3, cause 2).
* `onic_init_hardware()`: an FMAP programming failure jumps to
  `onic_clear_hardware()` before `hw->qdma` is set, which then uses a NULL
  `qdma_dev`. The new detection error path does its own cleanup and avoids
  this.
* QDMA 4.0 path: the `0xB08`/`0xE24` QDMA 3.x encodings and the undocumented
  error-mask bits are kept to preserve the baseline. Changing them needs a
  4.0 hardware test.
* `C2H_WRB_COAL_CFG.max_buf_sz` receives 128 in a 6-bit field and becomes 0.
  REF does the same; behaviour on hardware is unknown.
* `onic_netdev.c:1196` unused variable (fails `W=1` with `-Werror`).
