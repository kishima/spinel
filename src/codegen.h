#ifndef SPINEL_CODEGEN_H
#define SPINEL_CODEGEN_H

#include "node_table.h"

/* Library mode. When g_no_main is non-zero, codegen_program emits a non-static
   `int g_entry_name(void)` entry point instead of `int main(int,char**)`, for
   linking the compiled program into a host binary (embedded / FreeRTOS task).
   ARGV/$0 setup and atexit registration are omitted; END blocks run inline
   before the entry returns. Set these before calling codegen_program. */
extern int g_no_main;
extern const char *g_entry_name;

/* Persistent statics (`--persistent-statics`, library mode). A generated entry
   normally clears this TU's file-scope statics -- globals, top-level
   constants, class ivar caches, object pools -- every time it runs, because
   one entry call is one run of the program and the previous run's pointers
   belong to a heap that may be gone. A program called over and over as a
   library wants the opposite: the objects it built last call should still be
   there, the way an ordinary Ruby object outlives the method that made it.

   Set this and the clear happens once per instance rather than once per call,
   gated on sp_ctx's statics_inited. Sound only when one generated TU owns the
   instance -- which is already the condition for the file-scope statics to
   mean anything, since two programs in one instance would be sharing them. */
extern int g_persistent_statics;

/* Generate the full C translation unit for the program in `nt`.
   Returns a malloc'd NUL-terminated buffer (caller frees). Aborts the
   process with a diagnostic on an unsupported construct. */
char *codegen_program(const NodeTable *nt);

#endif
