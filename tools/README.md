# HALTZ host tools

Python tools that run on the PC (serial monitors, protocol sniffers, code generators that read
`shared/haltz_proto/include/haltz/msg_ids.def`).

| Tool | What it does |
|---|---|
| `gen_gpu_tables.py` | Writes `shared/haltz_proto/src/gpu_tables.c` (GPU default palette and font) from the palette in the script and `shared/haltz_proto/assets/font8x8.txt`. Run it after editing either; `--check` only tells whether the C file is up to date. Standard library only, no venv needed: `python3 tools/gen_gpu_tables.py` |

Set up a virtual environment once:

```bash
python3 -m venv tools/.venv
```

```bash
tools/.venv/bin/pip install -r tools/requirements.txt
```
