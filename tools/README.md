# HALTZ host tools

Python tools that run on the PC (serial monitors, protocol sniffers, code generators that read
`shared/haltz_proto/include/haltz/msg_ids.def`).

| Tool | What it does |
|---|---|
| `hzasm.py` | Assembler for the HALTZ VM (`docs/vm-v1.md` section 9). Reads `vm_opcodes.def` and `vm_syscalls.def`, writes a `.hzp` program image (header + body). `python3 tools/hzasm.py prog.s` (`-l -` prints a listing). Run the result with `build-host/hzrun` |
| `test_hzasm.py` | Assembler tests together with the C VM: every instruction assembled, disassembled by `hzrun -d` and assembled again gives the same bytes; the examples run. `python3 tools/test_hzasm.py` |
| `vm-examples/` | Example VM programs: `hello.s` (text, loop, recursive factorial), `echo.s` (keys, FRAME) |
| `gen_gpu_tables.py` | Writes `shared/haltz_proto/src/gpu_tables.c` (GPU default palette and font) from the palette in the script and `shared/haltz_proto/assets/font8x8.txt`. Run it after editing either; `--check` only tells whether the C file is up to date. Standard library only, no venv needed: `python3 tools/gen_gpu_tables.py` |

Set up a virtual environment once:

```bash
python3 -m venv tools/.venv
```

```bash
tools/.venv/bin/pip install -r tools/requirements.txt
```
