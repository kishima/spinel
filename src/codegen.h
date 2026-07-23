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

/* Generate the full C translation unit for the program in `nt`.
   Returns a malloc'd NUL-terminated buffer (caller frees). Aborts the
   process with a diagnostic on an unsupported construct. */
char *codegen_program(const NodeTable *nt);

#endif
