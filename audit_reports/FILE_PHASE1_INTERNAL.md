# Internal file/storage audit — phase 1

Baseline: `267160a0df44736b13437f8b3ea07e649ae9eb39`. The separate auditor prompts in `audit_prompts/file_phase1/` target the later fix commit identified in their text. This report records work done by the primary agent, not results from the three external auditors.

## Confirmed and fixed: FatFS rename-over deletes unrelated files

The FatFS adapter's rename-over fallback unconditionally deleted `~fmrn.tmp` in the destination directory before moving the existing destination there. A normal user file with that name was deleted. If that name was the source, the source could be destroyed and the operation could appear to succeed with the old destination content. The behavior was reproduced against the existing FatFS mock before the fix (two new tests failed in each mock generation) and against real ST FatFS on an in-memory disk (baseline regression exited 3). No board was involved.

The adapter now attempts 256 distinct 8.3-compatible `~fmXX.tmp` names and reserves one using FatFS rename; it does not remove occupied names. On exhaustion it returns `FM_ERR_EXISTS` without moving either source or destination. Tests cover the prior reserved name, occupied new names, source named like the old temp, and injected exhaustion. `test/build_fatfs_host.sh` passes with ASan/UBSan on classic, modern and legacy mocks and two-volume mock (51/51, 51/51, 51/51, 56/56). The optional real FatFS host probe `tools/check_fatfs_real_rename.sh` also passed with ST `stm32-mw-fatfs` source commit `dd9126e957ae841a13ccbe2115d39a1fb7414126`.

**Not a claim of crash atomicity:** FatFS rename-over still consists of several filesystem operations. A power loss or failed rollback may leave the destination aside. The header and documentation now state this limitation; the new bounded name selection is not a journal or automatic crash recovery. External concurrent actors bypassing the FileManager mount lock and real card failure timing were not verified.

## Further leads for the independent audits

The deterministic `<path>.fm_tmp` and backup names used by safe replacement/recovery and other helpers require a separate ownership, collision, concurrent-use and crash-recovery review; no claim is made here that every such path is safe. Path aliasing, VFS mount concurrency, other backends, real storage failure sweeps, portable ABI/alignment and the scope of any future non-ARM port remain independent audit topics. The prompts ask the auditors to test rather than infer these properties. No claim of defect-free or exhaustive coverage is possible from one host pass.
