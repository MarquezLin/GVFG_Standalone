# GigabyteLib dependency

This directory contains the supervisor-provided static MSVC library used by the
GVFG backend.

- Upstream version macro: `GVFG_SDK_VER 1.0.2`
- Library: `GvfgSdk.lib`
- SHA-256: `EC9577D4BE76861E44DFDD3B537DF2A57CCDA5A5F1B14E0BAAC92213E796A743`
- Original source: `ex_demo/GigabyteLib/vfg100_0914_b`

`GvfgSdk.lib` is linked into `gvfg.dll`; it is not distributed as a separate
customer runtime dependency. The supplied package did not contain source,
symbols, or a license file. Replace the library and headers together when a new
upstream build is delivered, and update the checksum above.
