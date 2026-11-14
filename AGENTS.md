# Agent entry point

Before changing this repository, start at [`project_memory/README.md`](project_memory/README.md). It routes tasks to the smallest relevant source files, configuration, tests, board glue and known constraints. The graph is a navigation aid, not a substitute for reading the specific code being changed.

When changing first-party code, interfaces, build settings or tests:
1. Run `python3 project_memory/graph.py check` to see whether the inventory is stale.
2. Review and update the relevant hand-maintained maps in `project_memory/` when behavior, ownership, dependencies or test coverage changes.
3. Run `python3 project_memory/graph.py update`, `python3 project_memory/test_graph.py`, then `python3 project_memory/graph.py check`; include the generated changes with the work.
4. Preserve unrelated user changes and generated/vendor files; do not infer correctness, runtime call order or target memory layout solely from lexical includes.

Documentation-only changes that do not affect the architecture should still keep these links valid. The check detects changes within its selected first-party file scope; it cannot judge whether the prose is semantically complete.
