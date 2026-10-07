# Deliberately broken programs

Every file here fails, on purpose. They exist so the quality of the diagnostics
can be seen (and tested) rather than assumed. `tests/integration/test_pipeline.cpp`
asserts that each one is rejected.

Try them:

```
minitool assemble examples/errors/unknown-opcode.asm -o /tmp/out.mobj
minitool build examples/errors/undefined-symbol.asm -o /tmp/out.mexe
```
