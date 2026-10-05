# Vendored LibreCAD sources

Reference copies, not part of any build. They are here so the claims in `docs/findings.md` can be
rechecked without network access, and so a future LibreCAD update can be diffed against what this
project was written for.

`librecad-v2.2.1.5/` — fetched from `github.com/LibreCAD/LibreCAD` at tag `v2.2.1.5`:

| File | Upstream path |
|---|---|
| `doc_plugin_interface.h` | `librecad/src/main/doc_plugin_interface.h` |
| `doc_plugin_interface.cpp` | `librecad/src/main/doc_plugin_interface.cpp` |
| `sample.cpp` | `plugins/sample/sample.cpp` |

`doc_plugin_interface.cpp` is the concrete implementation of the `Document_Interface` the plugin
talks to; almost everything in `docs/findings.md` about undo behaviour, argument units, and entity
lifetimes was read out of it.

The plugin-facing headers are not copied here. `/usr/include/librecad/document_interface.h` is
byte-identical to the upstream file at this tag, so the installed copy is the reference.

LibreCAD is GPL-2.0-or-later; these files carry their original notices.
