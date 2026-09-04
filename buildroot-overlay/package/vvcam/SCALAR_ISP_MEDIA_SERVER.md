# Scalar ISP media server

`isp_media_server_scalar` is the historical `isp_media_server` blob from
commit `25a6f59170640bbd9779f9a24167f1451fb2370f`
(`fix v4l2 run err`), blob ID
`00d911824c59ff7df4168500360fc875476d669a`. This is the exact daemon
inherited by the tip of `opt_linux_on_small_core_cherry-picked`, where the
CSI/ISP path was exercised on the scalar core.

Its ELF attributes declare RVV because it was compiled with an RVV-capable
`-march`, but complete disassembly contains no vector instructions. The name
`scalar` describes its executable instruction stream rather than its
over-broad ELF attribute.

Commit `83fce3541008bf5f6de9a0d3e7b312cb50ba1e19`
(`libmmz: Add kd_mpi_get_vvcam_video00()...`) replaced it with the first
daemon containing actual vector instructions. That revision and every later
daemon through the current blob contain 1458 vector instructions. On the
small core, the current daemon faults on instruction word `0xcc747057`,
which disassembles as `vsetivli zero,8,e8,mf2,ta,ma`.

Buildroot selects the scalar file when `BR2_RISCV_ISA_RVV` is disabled and
installs it as `/usr/bin/isp_media_server`. RVV-enabled configurations retain
the current daemon.

The daemon is a Canaan-supplied stripped prebuilt; source is not present in
this repository. Features added from `83fce35` onward require regression
